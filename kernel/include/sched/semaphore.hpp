/// @file semaphore.hpp
/// @brief A counter of things that are available, which threads can wait for.
///
/// With tokens to go around, taking and giving one is a single atomic operation. Anything more goes
/// through a WaitQueue. There is no owner, so there is no priority inheritance either. For mutual
/// exclusion, a Mutex is the better tool.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstddef>

#include "sched/wait_queue.hpp"

namespace kernel::sched {
/// @brief A counting semaphore.
///
/// Like the Mutex, one cannot be used to signal that it can go away: release() can still be
/// working on it when the thread it woke is done. sched::Completion is made for that.
class Semaphore {
  std::atomic<std::ptrdiff_t> m_count;
  WaitQueue m_waiters{};

  void pass_on() noexcept;

 public:
  /// @param initial How many tokens are available to begin with.
  constexpr explicit Semaphore(const std::ptrdiff_t initial = 0) noexcept : m_count(initial) {}

  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;

  /// @brief Takes a token, waiting for one if it has to.
  /// @pre The calling thread can sleep: see hw::percpu::can_sleep().
  void acquire() noexcept;

  /// @brief Takes a token if there is one. Never waits, and is safe from interrupt handlers.
  [[nodiscard]] bool try_acquire() noexcept;

  /// @brief Takes a token, unless that takes longer than @p timeout.
  /// @return Whether it got one. @pre As for acquire().
  [[nodiscard]] bool try_acquire_for(klib::Duration timeout) noexcept;

  /// @brief Gives back @p count tokens. Safe from interrupt handlers.
  void release(std::ptrdiff_t count = 1) noexcept;

  /// @brief How many tokens there are. Out of date by the time it gets to you.
  [[nodiscard]] std::ptrdiff_t available() const noexcept {
    return m_count.load(std::memory_order_relaxed);
  }
};
}  // namespace kernel::sched
