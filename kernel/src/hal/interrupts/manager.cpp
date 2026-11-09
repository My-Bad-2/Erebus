#include "hal/apic/ioapic.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::hw::interrupts {
namespace {
// Keeps register_handler() and unregister_handler() out of each other's way. Interrupts never
// take it: they only read, in RCU read sections.
constinit utils::QSpinlock g_update_lock{};

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
  KASSERT(reg != nullptr);
  utils::IrqSaveGuard guard{g_update_lock};
  KASSERT(!reg->m_registered);
  reg->m_registered = true;

  // Linked in before it is published, and the store that publishes it is a release, so an
  // interrupt that finds it finds all of it.
  utils::rcu::Ptr<InterruptRegistration>& head = s_slots[vector].handlers;
  reg->m_next.store(head.load_writer_locked());
  head.store(reg);
}

void InterruptManager::unregister_handler(const std::uint8_t vector,
                                          InterruptRegistration* reg) noexcept {
  KASSERT(percpu::can_sleep());

  {
    utils::IrqSaveGuard guard{g_update_lock};
    KASSERT(reg->m_registered);

    utils::rcu::Ptr<InterruptRegistration>* link = &s_slots[vector].handlers;
    for (InterruptRegistration* at = link->load_writer_locked(); at != reg;
         at = link->load_writer_locked()) {
      if (at == nullptr) [[unlikely]] {
        utils::logger::fatal("Interrupts: unregistering a handler vector {} does not have\n",
                             vector);
      }

      link = &at->m_next;
    }

    // An interrupt that is at it right now goes on from it to the rest, so its own link stays.
    link->store(reg->m_next.load_writer_locked());
    reg->m_registered = false;
  }

  // Every interrupt that could still see it, and every call to it, is in a read section that
  // started before it was unlinked.
  utils::rcu::synchronize();

  // No new interrupt gets to it from here on, so this ends.
  reg->m_dpc.flush();
}

void InterruptManager::route_event(const Event& event) noexcept {
  if (event.category == EventClass::Interrupt) [[likely]] {
    // The local APIC raises the spurious vector when an interrupt goes away before it could be
    // delivered. Nothing is in service, so there is nothing to acknowledge either.
    if (event.vector == std::to_underlying(InterruptVector::LocalApicSpurious)) [[unlikely]] {
      return;
    }

    percpu::hardirq_enter();
    const VectorSlot& slot = s_slots[event.vector];

    bool handled = false;
    bool yield = false;

    // The handlers run in the read section, so that unregister_handler() can wait for them. It
    // ends before the deferred calls and the scheduler get their turn below.
    {
      utils::rcu::ReadGuard rcu;
      for (InterruptRegistration* reg = slot.handlers.load(rcu); reg != nullptr;
           reg = reg->m_next.load(rcu)) {
        const InterruptStatus status = reg->m_callback(event, reg->m_ctx);

        if (sched::Dpc* deferred = reg->deferred_call();
            status == InterruptStatus::ScheduleDPC && deferred != nullptr) [[unlikely]] {
          (void)deferred->queue();
          handled = true;
          continue;
        }

        handled |= status != InterruptStatus::Unhandled;
        yield |= status == InterruptStatus::YieldRequested;
      }
    }

    percpu::lapic().eoi();

    if (slot.is_level_triggered) [[unlikely]] {
      if (slot.ioapic_controller) {
        slot.ioapic_controller->send_eoi(event.vector);
      }
    }

    if (!handled) [[unlikely]] {
      // Spurious, or for a handler that has just gone.
    }

    if (yield) [[unlikely]] {
      percpu::request_reschedule();
    }

    percpu::hardirq_exit();

    // This thread's stack carries the interrupt frame, so it can simply be left here and come back
    // to it later, and deferred calls can run on top of it. The interrupt is over as far as the
    // APIC is concerned. Only the outermost interrupt gets here with no blockers.
    if (percpu::ast_pending()) [[unlikely]] {
      sched::Scheduler::handle_ast();
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

void InterruptManager::handle_exception(const Event& event) noexcept {
  // Only the lookup is in a read section, and the handler is called after it: a page fault may
  // sleep, and may happen inside a read section already. Exception handlers stay registered for
  // good, so what was read stays good.
  Callback callback = nullptr;
  void* ctx = nullptr;
  {
    utils::rcu::ReadGuard rcu;
    if (const InterruptRegistration* reg = s_slots[event.vector].handlers.load(rcu)) {
      callback = reg->m_callback;
      ctx = reg->m_ctx;
    }
  }

  if (callback && callback(event, ctx) != InterruptStatus::Unhandled) {
    return;
  }

  const auto vector = static_cast<InterruptVector>(event.vector);
  if (event.cpl == EventCpl::Ring3 && vector != InterruptVector::MachineCheck &&
      vector != InterruptVector::DoubleFault) {
    if (const UserFaultHandler handler = s_user_fault_handler.load(std::memory_order_acquire)) {
      handler(event);
      return;
    }
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