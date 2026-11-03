#include "hal/apic/ioapic.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::interrupts {
namespace {
[[nodiscard]] constexpr const char* to_string(const EventClass category) noexcept {
  switch (category) {
    case EventClass::Interrupt:
      return "Interrupt (External)";
    case EventClass::Exception:
      return "Exception (Hardware/Software)";
    case EventClass::NMI:
      return "NMI (Non-Maskable)";
    case EventClass::Syscall:
      return "System Call (SYSCALL/SYSENTER/INT n)";
    default:
      return "Unknown";
  }
}

void dump_event(const Event& event) noexcept {
  utils::logger::debug("================== CPU EVENT DUMP ==================\n");

  utils::logger::debug("Category:   {}\n", to_string(event.category));
  utils::logger::debug("Vector:     {} ({:#04x})\n", event.vector, event.vector);
  utils::logger::debug("CPL:        Ring {}\n", std::to_underlying(event.cpl));
  utils::logger::debug("Error Code: {:#06x}\n", event.error_code);

  utils::logger::debug("Payload:    {:#018x}\n", event.payload);
  utils::logger::debug("----------------------------------------------------\n");

  utils::logger::debug("RIP:        {:#018x}\n", event.rip);
  utils::logger::debug("RSP:        {:#018x}\n", event.rsp);
  utils::logger::debug("RFLAGS:     {:#018x}\n", event.rflags);

  if (event.gprs != nullptr) {
    utils::logger::debug("--------------------- GPR STATE --------------------\n");
    utils::logger::debug("RAX: {:#018x}  RBX: {:#018x}  RCX: {:#018x}\n", event.gprs->rax,
                         event.gprs->rbx, event.gprs->rcx);
    utils::logger::debug("RDX: {:#018x}  RSI: {:#018x}  RDI: {:#018x}\n", event.gprs->rdx,
                         event.gprs->rsi, event.gprs->rdi);
    utils::logger::debug("RBP: {:#018x}  R8:  {:#018x}  R9:  {:#018x}\n", event.gprs->rbp,
                         event.gprs->r8, event.gprs->r9);
    utils::logger::debug("R10: {:#018x}  R11: {:#018x}  R12: {:#018x}\n", event.gprs->r10,
                         event.gprs->r11, event.gprs->r12);
    utils::logger::debug("R13: {:#018x}  R14: {:#018x}  R15: {:#018x}\n", event.gprs->r13,
                         event.gprs->r14, event.gprs->r15);
  }

  utils::logger::debug("====================================================\n");
}
}  // namespace

void InterruptManager::set_eoi_strategy(const std::uint8_t vector, const bool is_lvl,
                                        apic::IoApicController* controller) noexcept {
  s_slots[vector].is_level_triggered = is_lvl;
  s_slots[vector].ioapic_controller = controller;
}

void InterruptManager::register_handler(const std::uint8_t vector,
                                        InterruptRegistration* reg) noexcept {
  auto& [lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] =
      s_slots[vector];

  utils::IrqSaveGuard guard(lock);

  const bool was_empty = list.empty();
  list.push_front(*reg);

  if (was_empty) [[likely]] {
    direct_callback = reg->m_callback;
    direct_dpc = reg->m_dpc;
    direct_ctx = reg->m_ctx;
  } else {
    direct_callback = nullptr;
  }
}

void InterruptManager::unregister_handler(const std::uint8_t vector,
                                          InterruptRegistration* reg) noexcept {
  auto& [lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] =
      s_slots[vector];
  utils::IrqSaveGuard guard(lock);
  list.remove(*reg);

  const auto it = list.begin();
  if (it != list.end()) {
    auto next = it;
    ++next;

    if (next == list.end()) {
      direct_callback = it->m_callback;
      direct_dpc = it->m_dpc;
      direct_ctx = it->m_ctx;
      return;
    }
  }

  direct_callback = nullptr;
}

void InterruptManager::route_event(const Event& event) noexcept {
  if (event.category == EventClass::Interrupt) [[likely]] {
    // The local APIC raises the spurious vector when an interrupt goes away before it could be
    // delivered. Nothing is in service, so there is nothing to acknowledge either: an EOI here
    // would retire some other interrupt.
    if (event.vector == std::to_underlying(InterruptVector::LocalApicSpurious)) [[unlikely]] {
      return;
    }

    percpu::hardirq_enter();

    auto& [lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] =
        s_slots[event.vector];

    bool handled = false;
    bool yield = false;

    // A single handler is called without the slot lock, so the same vector can be handled on
    // every CPU at once and the handler is free to take locks or wait for other CPUs (TLB
    // shootdowns do). Registration happens at init time; a snapshot is all that is needed.
    Callback callback;
    DpcCallback dpc;
    void* ctx;
    {
      utils::NakedGuard guard(lock);
      callback = direct_callback;
      dpc = direct_dpc;
      ctx = direct_ctx;
    }

    if (callback != nullptr) [[likely]] {
      const InterruptStatus status = callback(event, ctx);

      if (status == InterruptStatus::ScheduleDPC && dpc) [[unlikely]] {
        // Queue a DPC
        handled = true;
      } else {
        handled = status != InterruptStatus::Unhandled;
        yield = status == InterruptStatus::YieldRequested;
      }
    } else {
      // Shared vectors walk the list, which needs the lock.
      utils::NakedGuard guard(lock);
      for (auto& reg : list) {
        InterruptStatus status = reg.m_callback(event, reg.m_ctx);

        if (status == InterruptStatus::ScheduleDPC && reg.m_dpc) [[unlikely]] {
          // Queue a DPC
          handled = true;
          continue;
        }

        handled |= (status != InterruptStatus::Unhandled);
        yield |= (status == InterruptStatus::YieldRequested);
      }
    }

    percpu::lapic().eoi();

    if (lvl_triggered) [[unlikely]]
      if (ioapic) ioapic->send_eoi(event.vector);

    if (!handled && list.empty()) [[unlikely]] {
      // Spurious interrupt
    }

    // Delegate to scheduler if a driver unblocked a high-prio thread
    if (yield) [[unlikely]] {
      // yield
    }

    percpu::hardirq_exit();
    return;
  }

  if (event.category == EventClass::Exception) [[unlikely]] {
    handle_exception(event);
  } else if (event.category == EventClass::NMI) [[unlikely]] {
    handle_nmi(event);
  } else if (event.category == EventClass::Syscall) [[unlikely]] {
    // Dispatch syscalls
  }
}

void InterruptManager::handle_exception(const Event& event) noexcept {
  // Never run the handler under the slot lock: page faults happen on every CPU at once, may
  // nest, and may have to wait for other CPUs (to shoot down TLB entries), any of which would
  // deadlock against a lock held across the call.
  Callback callback = nullptr;
  void* ctx = nullptr;
  {
    auto& slot = s_slots[event.vector];
    utils::NakedGuard guard(slot.lock);

    const auto it = slot.list.begin();
    if (it != slot.list.end()) {
      callback = it->m_callback;
      ctx = it->m_ctx;
    }
  }

  if (callback && callback(event, ctx) != InterruptStatus::Unhandled) {
    return;
  }

  dump_event(event);
  utils::logger::fatal("Unhandled exception: {}\n", event.vector);
}

void InterruptManager::handle_nmi(const Event& event) noexcept {
  percpu::nmi_enter();

  // A panicking CPU stops the others with NMIs; those never come back from here.
  if (smp::handle_stop_nmi()) {
    std::unreachable();
  }

  dump_event(event);
  utils::logger::fatal("NMI triggered!\n");
}
}  // namespace kernel::hw::interrupts