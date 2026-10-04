#include "hal/apic/ioapic.hpp"
#include "hal/interrupt_manager.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::interrupts {
namespace {
[[nodiscard]] constexpr const char *to_string(const EventClass category) noexcept {
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

void dump_event(const Event &event) noexcept {
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
    utils::logger::debug("RAX: {:#018x}  RBX: {:#018x}  RCX: {:#018x}\n", event.gprs->rax, event.gprs->rbx,
                         event.gprs->rcx);
    utils::logger::debug("RDX: {:#018x}  RSI: {:#018x}  RDI: {:#018x}\n", event.gprs->rdx, event.gprs->rsi,
                         event.gprs->rdi);
    utils::logger::debug("RBP: {:#018x}  R8:  {:#018x}  R9:  {:#018x}\n", event.gprs->rbp, event.gprs->r8,
                         event.gprs->r9);
    utils::logger::debug("R10: {:#018x}  R11: {:#018x}  R12: {:#018x}\n", event.gprs->r10, event.gprs->r11,
                         event.gprs->r12);
    utils::logger::debug("R13: {:#018x}  R14: {:#018x}  R15: {:#018x}\n", event.gprs->r13, event.gprs->r14,
                         event.gprs->r15);
  }

  utils::logger::debug("====================================================\n");
}
} // namespace

void InterruptManager::set_eoi_strategy(const std::uint8_t vector, const bool is_lvl,
                                        apic::IoApicController *controller) noexcept {
  s_slots[vector].is_level_triggered = is_lvl;
  s_slots[vector].ioapic_controller = controller;
}

void InterruptManager::register_handler(const std::uint8_t vector, InterruptRegistration *reg) noexcept {
  auto &[lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] = s_slots[vector];

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

void InterruptManager::unregister_handler(const std::uint8_t vector, InterruptRegistration *reg) noexcept {
  auto &[lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] = s_slots[vector];
  utils::IrqSaveGuard guard(lock);
  list.remove(*reg);

  auto it = list.begin();
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

void InterruptManager::route_event(const Event &event) noexcept {
  if (event.category == EventClass::Interrupt) [[likely]] {
    auto &[lock, direct_callback, direct_dpc, direct_ctx, list, lvl_triggered, ioapic] = s_slots[event.vector];

    bool handled = false;
    bool yield = false;
    {
      utils::NakedGuard guard(lock);

      if (direct_dpc != nullptr) [[likely]] {
        const InterruptStatus status = direct_callback(event, direct_ctx);

        if (status == InterruptStatus::ScheduleDPC && direct_dpc) [[unlikely]] {
          // Queue a DPC
          handled = true;
        } else {
          handled = status != InterruptStatus::Unhandled;
          yield = status == InterruptStatus::YieldRequested;
        }
      } else {
        for (auto &reg : list) {
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
    }

    percpu::lapic().eoi();

    if (lvl_triggered) [[unlikely]] {
      if (ioapic) {
        ioapic->send_eoi(event.vector);
      }
    }

    if (!handled && list.empty()) [[unlikely]] {
      // Spurious interrupt
    }

    // Delegate to scheduler if a driver unblocked a high-prio thread
    if (yield) [[unlikely]] {
      // yield
    }

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

void InterruptManager::handle_exception(const Event &event) noexcept {
  auto &slot = s_slots[event.vector];

  {
    utils::NakedGuard guard(slot.lock);
    auto it = slot.list.begin();

    if (it != slot.list.end()) {
      if (it->m_callback(event, it->m_ctx) != InterruptStatus::Unhandled) {
        return;
      }
    }
  }

  dump_event(event);
  utils::logger::fatal("Unhandled exception: {}\n", event.vector);
}

void InterruptManager::handle_nmi(const Event &event) noexcept {
  dump_event(event);
  utils::logger::fatal("NMI triggered!\n");
}
} // namespace kernel::hw::interrupts