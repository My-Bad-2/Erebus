/// @file sleep.hpp
/// @brief Letting a thread sleep until a moment in time.
///
/// @code
/// sched::sleep_for(klib::Duration::milliseconds(5));
/// @endcode
///
/// There is no queue of sleepers. hw::timer already keeps a sorted list of deadlines per CPU and
/// programs the hardware for the first one, so a sleep is an Alarm on the sleeper's stack: one
/// timer that wakes one thread. The same Alarm is what puts a limit on any other wait.
#pragma once

#include <atomic>
#include <chrono.hpp>

#include "hal/timer/timer.hpp"

namespace kernel::sched {
class Thread;

/// @brief Wakes the thread that set it, at a moment in time.
///
/// This is all a timeout comes down to: whoever waits checks rang() along with whatever else it
/// waits for. So the timer's interrupt handler never has to touch the thing being waited on, and
/// the thread that was waiting cleans up after itself. Letting it go out of scope is enough to
/// stop it, including when it is going off on another CPU at that moment, and that is also how it
/// is safe on a stack.
class Alarm {
  hw::timer::Timer m_timer{};
  Thread* m_thread{nullptr};
  std::atomic<bool> m_rang{false};

  static hw::timer::TimerAction ring(hw::timer::Timer& timer, void* alarm) noexcept;

 public:
  /// @brief An alarm that does not go off. For code that may or may not have a deadline.
  Alarm() noexcept = default;

  explicit Alarm(const klib::TimePoint deadline, const klib::Duration slack = {}) noexcept {
    set(deadline, slack);
  }

  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;

  ~Alarm() noexcept { m_timer.cancel_sync(); }

  /// @brief Makes it go off at @p deadline, or right away if that is in the past.
  /// @param deadline Not before this.
  /// @param slack    How long after @p deadline is fine too, if that saves an interrupt.
  /// @pre It was not set before.
  void set(klib::TimePoint deadline, klib::Duration slack = {}) noexcept;

  /// @brief Whether it went off. A thread about to block looks at this after prepare_to_block(), so
  ///        that an alarm going off at the same moment either shows here or finds it blocked.
  [[nodiscard]] bool rang() const noexcept { return m_rang.load(std::memory_order_acquire); }
};

/// @brief Sleeps until @p deadline.
/// @param deadline Not before this, unless cancel_sleep() gets in the way.
/// @param slack    How long after @p deadline is fine too, if that saves an interrupt.
/// @return false if cancel_sleep() cut it short.
/// @pre The calling thread can sleep: see hw::percpu::can_sleep().
bool sleep_until(klib::TimePoint deadline, klib::Duration slack = {}) noexcept;

/// @brief sleep_until(), @p duration from now.
bool sleep_for(klib::Duration duration, klib::Duration slack = {}) noexcept;

/// @brief Ends the sleep of @p thread, or its next one if it is not sleeping.
///
/// The second part is what makes this reliable: a thread that is just about to go to sleep has not
/// missed it. It does not wake a thread that waits for anything else.
/// @pre @p thread is not gone yet.
void cancel_sleep(Thread* thread) noexcept;
}  // namespace kernel::sched
