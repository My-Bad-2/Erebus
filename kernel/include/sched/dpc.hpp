/// @file dpc.hpp
/// @brief Deferred calls: the part of an interrupt handler that can wait a moment.
///
/// @code
/// InterruptStatus on_interrupt(const Event&, void* device) noexcept {
///   acknowledge(device);
///   return InterruptStatus::ScheduleDPC;
/// }
///
/// void process_completions(void* device) noexcept { ... }
///
/// constinit InterruptRegistration g_handler{&on_interrupt, &g_device, &process_completions};
/// @endcode
///
/// An interrupt handler runs with interrupts off, so whatever it does delays everyone else's
/// interrupts. A deferred call runs on the same CPU shortly after, as the interrupt ends, with
/// interrupts back on, so all that is held up is the thread that was interrupted.
///
/// ### When
///
/// On the way out of an interrupt, or of a section with preemption off, as long as nothing else is
/// in the way: not in another interrupt or deferred call, and not with preemption off. That last
/// part matters. It means that a deferred call only ever interrupts code that could have lost the
/// CPU at that point anyway, so a lock held with preemption off needs nothing more to be safe from
/// these, and the lock can be taken here too.
///
/// A deferred call runs in interrupt context as far as anyone can tell, on top of whatever thread
/// it interrupted. It cannot sleep or wait for that thread, and may be interrupted by anything.
///
/// ### Why not a queue of function pointers
///
/// A queue has a size, and a full one has nothing good to do with the call it cannot take: it has
/// already told the device that everything is fine. A Dpc is an object that belongs to whoever
/// queues it, so that is one link and one flag, and queueing it can never fail. An interrupt that
/// arrives while the call is still waiting does not queue another one, which is the right thing
/// too, since what it has to process will be there when the call gets around to it.
#pragma once

#include <atomic>
#include <cstdint>

#include "hal/timer/timer.hpp"
#include "utils/linked_list.hpp"

namespace kernel::sched {
/// @brief A call to make soon, on the CPU that asked for it.
///
/// Runs on one CPU at a time. A Dpc queued again while it is running runs again afterwards. If that
/// happens on another CPU, that CPU waits for its turn without holding anything up.
class Dpc : public utils::SListNode<Dpc> {
 public:
  using Callback = void (*)(void* context);

 private:
  friend class DpcQueue;

  static constexpr std::uint8_t QUEUED = 1u << 0;
  static constexpr std::uint8_t RUNNING = 1u << 1;

  Callback m_callback{nullptr};
  void* m_context{nullptr};
  std::atomic<std::uint8_t> m_state{0};

 public:
  constexpr explicit Dpc(const Callback callback = nullptr, void* context = nullptr) noexcept
      : m_callback(callback), m_context(context) {}

  Dpc(const Dpc&) = delete;
  Dpc& operator=(const Dpc&) = delete;

  /// @brief Whether there is anything to call.
  [[nodiscard]] constexpr bool has_callback() const noexcept { return m_callback != nullptr; }

  /// @brief Asks for the call on the calling CPU. Works from interrupt handlers and with locks
  /// held,
  ///        but not from an NMI, which can arrive in the middle of this.
  /// @return false if it was waiting already, in which case that is what will run.
  bool queue() noexcept;

  /// @brief Waits until nothing of it is left: not queued anywhere and not running. It can go away
  ///        then, as long as nothing queues it again.
  /// @pre The calling thread can sleep: see hw::percpu::can_sleep().
  void flush() const noexcept;

  /// @brief Whether it is waiting to run.
  [[nodiscard]] bool is_queued() const noexcept {
    return (m_state.load(std::memory_order_acquire) & QUEUED) != 0;
  }
};

/// @brief What a CPU has been asked to call. Lives in its hw::PerCpu.
class DpcQueue {
  // Looks again this many times for calls that came in during the last ones. After that, the rest
  // goes through a timer. Something that keeps queueing itself must not keep threads off the CPU.
  static constexpr std::uint32_t MAX_PASSES = 4;

  utils::SList<Dpc> m_pending{};
  Dpc* m_last{nullptr};
  hw::timer::Timer m_retry{};
  std::uint64_t m_calls{0};

  void add(Dpc& dpc) noexcept;
  void run(Dpc& dpc) noexcept;

  friend class Dpc;

 public:
  constexpr DpcQueue() noexcept = default;

  DpcQueue(const DpcQueue&) = delete;
  DpcQueue& operator=(const DpcQueue&) = delete;

  /// @brief Runs what is waiting. The way out of an interrupt comes here if AST_DPC_PENDING is up.
  /// @pre Interrupts are off, and hw::percpu::ast_pending() would say yes. On return too, though
  ///      they are on for most of what happens in between.
  void drain() noexcept;

  /// @brief How many calls this CPU has made.
  [[nodiscard]] std::uint64_t calls() const noexcept { return m_calls; }
};
}  // namespace kernel::sched
