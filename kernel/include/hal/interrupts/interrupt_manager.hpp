#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "interrupts.hpp"
#include "sched/dpc.hpp"
#include "utils/rcu/rcu_ptr.hpp"

namespace kernel::hw::apic {
class IoApicController;
}
namespace kernel::hw::interrupts {
enum class InterruptStatus : std::uint8_t {
  Handled,
  Unhandled,
  YieldRequested,
  ScheduleDPC,  // ISR requests Bottom-Half deferral
};

using Callback = InterruptStatus (*)(const Event&, void* ctx);
using DpcCallback = sched::Dpc::Callback;

/// @brief What happens to user code that faults in a way nothing handled. See
///        InterruptManager::set_user_fault_handler().
using UserFaultHandler = void (*)(const Event& event) noexcept;

/// @brief A handler on a vector, and what it defers. The caller owns it, and it has to stay where
///        it is from register_handler() until unregister_handler() has returned.
class InterruptRegistration {
  friend class InterruptManager;

 public:
  constexpr InterruptRegistration(const Callback cb, void* ctx,
                                  const DpcCallback dpc = nullptr) noexcept
      : m_callback(cb), m_ctx(ctx), m_dpc(dpc, ctx) {}

  InterruptRegistration(const InterruptRegistration&) = delete;
  InterruptRegistration& operator=(const InterruptRegistration&) = delete;

 private:
  // What a vector holds on to: nothing to queue for a handler that does not defer anything.
  [[nodiscard]] constexpr sched::Dpc* deferred_call() noexcept {
    return m_dpc.has_callback() ? &m_dpc : nullptr;
  }

  Callback m_callback{nullptr};
  void* m_ctx{nullptr};
  sched::Dpc m_dpc;

  // The next handler on the same vector. Interrupts follow it without a lock, so once this one is
  // unlinked it keeps pointing on to the rest until unregister_handler() is done waiting.
  utils::rcu::Ptr<InterruptRegistration> m_next{};
  bool m_registered{false};  ///< Under the update lock, to catch a registration made twice.
};

/// @brief Who handles each vector, and the way every interrupt and exception gets to them.
///
/// Every interrupt and every exception looks its handlers up here, on every CPU at once, while
/// handlers come and go a handful of times per boot. So looking up takes no lock: the handlers of
/// a vector are a list that interrupts walk in an RCU read section (utils/rcu/rcu.hpp). Updates
/// take the one lock there is, which no interrupt ever touches, and a handler that is taken off
/// waits out a grace period before the caller gets it back.
///
/// Interrupt handlers run inside that read section, which is what lets unregister_handler() wait
/// for the calls that are still going on. It costs them nothing, as they could not sleep anyway.
/// They may take locks and wait for other CPUs, as TLB shootdowns do, shared vector or not.
/// Exception handlers are called after it instead: a page fault may sleep.
class InterruptManager {
  // Not a cache line each: interrupts only read these, and packed they take a third of the cache.
  struct VectorSlot {
    utils::rcu::Ptr<InterruptRegistration> handlers;  ///< The newest first.
    bool is_level_triggered;
    apic::IoApicController* ioapic_controller;

    // Spelled out, as s_slots cannot use default member initializers of a class nested in the one
    // it is a member of.
    constexpr VectorSlot() noexcept
        : handlers(nullptr), is_level_triggered(false), ioapic_controller(nullptr) {}
  };

  static inline std::array<VectorSlot, IDT_SIZE> s_slots{};
  static inline std::atomic<UserFaultHandler> s_user_fault_handler{nullptr};

  static void handle_exception(const Event& event) noexcept;
  static void handle_nmi(const Event& event) noexcept;

 public:
  static void set_eoi_strategy(std::uint8_t vector, bool is_lvl,
                               apic::IoApicController* controller = nullptr) noexcept;

  /// @brief Adds @p reg to the handlers of @p vector. Interrupts call it from now on, before the
  ///        handlers that were there already.
  /// @pre @p reg is not registered on any vector.
  static void register_handler(std::uint8_t vector, InterruptRegistration* reg) noexcept;

  /// @brief Takes a handler off a vector, then waits for every call to it that is still going on
  ///        and for what it deferred. After that, @p reg is the caller's again.
  ///
  /// The one thing it cannot wait for is an exception handler that is already running, as those
  /// run outside the read section. Nothing takes one off.
  /// @pre The calling thread can sleep, and @p reg is registered on @p vector.
  static void unregister_handler(std::uint8_t vector, InterruptRegistration* reg) noexcept;

  /// @brief Where every event goes: interrupts to their handlers, exceptions to theirs or to the
  ///        user fault handler, and under FRED system calls to core::syscall::handle().
  static void route_event(const Event& event) noexcept;

  /// @brief Sets what is done about an exception from ring 3 that no handler took care of.
  ///
  /// A fault in the kernel is a bug in the kernel and ends everything. One in user code only means
  /// the end of that code: @p handler sees to it, on the faulting thread, which takes the way back
  /// to user mode afterward (see sched::Scheduler::return_to_user()). Machine checks and double
  /// faults never come here, as nothing a user did caused them.
  static void set_user_fault_handler(UserFaultHandler handler) noexcept {
    s_user_fault_handler.store(handler, std::memory_order_release);
  }
};
}  // namespace kernel::hw::interrupts