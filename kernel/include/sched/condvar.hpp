/// @file condvar.hpp
/// @brief Waiting for a condition that a Mutex protects.
///
/// @code
/// utils::NakedGuard guard{lock};
/// while (!queue_has_work()) {
///   not_empty.wait(lock);
/// }
/// @endcode
///
/// The caller goes on the queue before it lets go of the Mutex. So nobody can change the condition
/// and notify in between, which is the entire reason for having this.
///
/// A wake here is no promise about the condition, only that someone called notify: another
/// thread can get to the Mutex first and change everything. That is why the condition goes in a
/// loop, as above. There are no other wakeups: wait() does not come back for no reason.
#pragma once

#include <chrono.hpp>
#include <cstdint>

#include "sched/mutex.hpp"
#include "sched/wait_queue.hpp"

namespace kernel::sched {
/// @brief A condition variable. See the file comment.
class CondVar {
  WaitQueue m_waiters{};

  bool wait_locked(Mutex& mutex, const klib::TimePoint* deadline) noexcept;

 public:
  constexpr CondVar() noexcept = default;

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  /// @brief Lets go of @p mutex and waits for a notification, then takes @p mutex back.
  /// @pre The calling thread holds @p mutex and can otherwise sleep.
  void wait(Mutex& mutex) noexcept;

  /// @brief wait(), for no longer than @p timeout, and @p mutex is held again either way.
  /// @return Whether a notification came. If not, there is no way that one got lost: a notify_one()
  ///         that picked this thread counts as having arrived, however close the timeout was.
  [[nodiscard]] bool wait_for(Mutex& mutex, klib::Duration timeout) noexcept;

  /// @brief wait_for() up to a moment in time.
  [[nodiscard]] bool wait_until(Mutex& mutex, klib::TimePoint deadline) noexcept;

  /// @brief Wakes the thread that has been waiting longest. Safe from interrupt handlers.
  void notify_one() noexcept { (void)m_waiters.wake_one(); }

  /// @brief Wakes everyone. Safe from interrupt handlers.
  void notify_all() noexcept { (void)m_waiters.wake_all(); }

  /// @brief How many threads are waiting. Out of date by the time it gets to you.
  [[nodiscard]] std::uint32_t waiters() const noexcept { return m_waiters.waiters(); }
};
}  // namespace kernel::sched
