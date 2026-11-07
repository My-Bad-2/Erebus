#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "interrupts.hpp"
#include "sched/dpc.hpp"
#include "utils/linked_list.hpp"
#include "utils/lock.hpp"

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

class InterruptRegistration : public utils::SListNode<InterruptRegistration> {
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
};

class InterruptManager {
  struct alignas(std::hardware_destructive_interference_size) VectorSlot {
    utils::QSpinlock lock;

    Callback direct_callback;
    sched::Dpc* direct_dpc;
    void* direct_ctx;
    utils::SList<InterruptRegistration> list;

    bool is_level_triggered;
    apic::IoApicController* ioapic_controller;

    constexpr VectorSlot() noexcept
        : direct_callback(nullptr),
          direct_dpc(nullptr),
          direct_ctx(nullptr),
          is_level_triggered(false),
          ioapic_controller(nullptr) {}
  };

  static inline std::array<VectorSlot, IDT_SIZE> s_slots{};
  static inline std::atomic<UserFaultHandler> s_user_fault_handler{nullptr};

  static void handle_exception(const Event& event) noexcept;
  static void handle_nmi(const Event& event) noexcept;

 public:
  static void initialize() noexcept {
    for (auto& slot : s_slots) {
      slot.list.clear();
    }
  }

  static void set_eoi_strategy(std::uint8_t vector, bool is_lvl,
                               apic::IoApicController* controller = nullptr) noexcept;
  static void register_handler(std::uint8_t vector, InterruptRegistration* reg) noexcept;

  /// @brief Takes a handler off a vector, and waits for what it deferred to be over.
  /// @pre The calling thread can sleep, and @p reg is registered on @p vector.
  static void unregister_handler(std::uint8_t vector, InterruptRegistration* reg) noexcept;
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