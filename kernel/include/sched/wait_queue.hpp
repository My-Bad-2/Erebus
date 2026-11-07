/// @file wait_queue.hpp
/// @brief Threads waiting for something to become true.
///
/// @code
/// // One side
/// queue.wait([&] { return ready.load(std::memory_order_acquire); });
///
/// // The other, which can be an interrupt handler
/// ready.store(true, std::memory_order_release);
/// queue.wake_all();
/// @endcode
///
/// The condition is whatever the caller says it is, and has to be true by the time a wake call is
/// made. A wake that comes along at any moment of a wait cannot get lost: a waiter is on the queue
/// and marked as blocked before it evaluates the condition for the last time, so either that sees
/// the change, or the waker sees the waiter.
///
/// The waiting code evaluates the condition without holding the queue's lock. That keeps a slow
/// condition from getting in the way of the waker, and it is why a condition must not block,
/// since it is evaluated with the thread already marked as blocked.
///
/// Waiters leave in the order they came, and those are the only guarantees there are. A thread
/// that comes along at the wrong time can take what the first waiter was woken for, in which case
/// that one goes back to waiting, at the end of the line. A woken thread can also find its
/// condition false for the same reason, so waiters look again, which the functions below do.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <concepts>
#include <cstdint>
#include <limits>
#include <utility>

#include "drivers/clock.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"
#include "utils/lock.hpp"

namespace kernel::sched {
/// @brief A queue of waiting threads. See the file comment.
class WaitQueue {
 public:
  /// @brief A thread's place in line. Lives on its stack for as long as the wait takes.
  class Waiter {
    friend class WaitQueue;

    Waiter* m_prev{nullptr};
    Waiter* m_next{nullptr};
    Thread* m_thread{current()};
    std::atomic<bool> m_queued{false};

   public:
    Waiter() noexcept = default;

    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;

    /// @brief Whether it is still in line, so that no wake has come for it.
    [[nodiscard]] bool is_queued() const noexcept {
      return m_queued.load(std::memory_order_acquire);
    }
  };

 private:
  utils::QSpinlock m_lock{};
  std::atomic<std::uint32_t> m_count{0};
  Waiter* m_head{nullptr};
  Waiter* m_tail{nullptr};

  void link_locked(Waiter& waiter) noexcept;
  void unlink_locked(Waiter& waiter) noexcept;
  std::uint32_t collect_locked(WakeList& wakes, std::uint32_t max) noexcept;

 public:
  static constexpr std::uint32_t ALL = std::numeric_limits<std::uint32_t>::max();

  constexpr WaitQueue() noexcept = default;

  WaitQueue(const WaitQueue&) = delete;
  WaitQueue& operator=(const WaitQueue&) = delete;

  /// @brief Blocks until @p pred returns true. See the file comment for what it can do.
  /// @pre The calling thread can sleep: see hw::percpu::can_sleep().
  template <std::predicate Pred>
  void wait(Pred&& pred) noexcept {
    if (pred()) {
      return;
    }

    Waiter waiter;
    while (true) {
      prepare(waiter);
      if (pred()) {
        break;
      }

      Scheduler::schedule();

      // The usual way back here: a wake, for a condition that holds. This is what saves it from
      // going back in line, through the lock, only to leave again.
      if (!waiter.is_queued() && pred()) {
        break;
      }
    }

    (void)finish(waiter);
  }

  /// @brief Blocks until @p pred returns true, or until @p deadline.
  /// @return The last thing @p pred said. A wake spent on a thread that is timing out has to go
  ///         somewhere, so that gets one more look.
  template <std::predicate Pred>
  [[nodiscard]] bool wait_until(const klib::TimePoint deadline, Pred&& pred) noexcept {
    if (pred()) {
      return true;
    }

    const Alarm alarm{deadline};
    Waiter waiter;

    while (true) {
      prepare(waiter);
      if (pred()) {
        (void)finish(waiter);
        return true;
      }

      if (alarm.rang()) {
        (void)finish(waiter);
        return pred();
      }

      Scheduler::schedule();

      if (!waiter.is_queued() && pred()) {
        (void)finish(waiter);
        return true;
      }
    }
  }

  /// @brief wait_until(), @p timeout from now.
  template <std::predicate Pred>
  [[nodiscard]] bool wait_for(const klib::Duration timeout, Pred&& pred) noexcept {
    return wait_until(drivers::SystemClock::now() + timeout, std::forward<Pred>(pred));
  }

  /// @brief Goes into line, if it is not there, and marks the thread as blocked. The next
  ///        Scheduler::schedule() takes it off the CPU, unless it was woken by then.
  ///
  /// This and finish() are the wait functions above taken apart, for waits that have to do
  /// something in between: CondVar has to let go of a Mutex.
  /// @pre The calling thread owns @p waiter, and can sleep.
  void prepare(Waiter& waiter) noexcept;

  /// @brief Ends a wait: leaves the line, if it is still there.
  /// @return Whether a wake was spent on this waiter. A wait that gives up for another reason has
  ///         to pass that on to someone if it consumed a one-at-a-time wake.
  bool finish(Waiter& waiter) noexcept;

  /// @brief Wakes up to @p count waiters, the longest waiting first. Safe from interrupt handlers.
  /// @return How many were woken.
  std::uint32_t wake(std::uint32_t count) noexcept;

  std::uint32_t wake_one() noexcept { return wake(1); }
  std::uint32_t wake_all() noexcept { return wake(ALL); }

  /// @brief Wakes everyone, with @p update running under the queue's lock first.
  ///
  /// After the lock is gone, nothing here touches the queue again. That is the guarantee a
  /// short-lived object needs, as long as its waiters pass through synchronize() before they
  /// let it go: the moment they see what @p update did, the object can go away.
  template <std::invocable Update>
  std::uint32_t wake_all_after(Update&& update) noexcept {
    WakeList wakes;
    std::uint32_t woken = 0;

    hw::percpu::preempt_disable();
    {
      utils::IrqSaveGuard guard{m_lock};
      update();
      woken = collect_locked(wakes, ALL);
    }

    wakes.wake_all();
    hw::percpu::preempt_enable();
    return woken;
  }

  /// @brief Waits for whoever is inside wake_all_after() to be done with the queue.
  void synchronize() noexcept { utils::IrqSaveGuard guard{m_lock}; }

  /// @brief How many threads are in line. Out of date by the time it gets to you.
  [[nodiscard]] std::uint32_t waiters() const noexcept {
    return m_count.load(std::memory_order_relaxed);
  }
};
}  // namespace kernel::sched
