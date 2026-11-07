/// @file completion.hpp
/// @brief One thing happening once, for any number of threads to wait for.
///
/// @code
/// sched::Completion done;
/// start_request(&done);
/// done.wait();
/// // `done` can go away here, even with the other side still in complete_all()
/// @endcode
///
/// That last line is why this is not a Semaphore. Whoever is told that something finished often
/// frees what it was about, the Completion with it, while the thread that said so is still on its
/// way out of the call. complete_all() never touches the object after the waiters can see the
/// result, and wait() does not return before complete_all() is done with it.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstddef>

#include "sched/wait_queue.hpp"

namespace kernel::sched {
/// @brief Something that happens once. See the file comment.
class Completion {
  std::atomic<bool> m_done{false};
  WaitQueue m_waiters{};

  void settle() noexcept { m_waiters.synchronize(); }

 public:
  constexpr Completion() noexcept = default;

  Completion(const Completion&) = delete;
  Completion& operator=(const Completion&) = delete;

  /// @brief Waits until it happens, which it can have done already.
  /// @pre The calling thread can sleep: see hw::percpu::can_sleep().
  void wait() noexcept {
    m_waiters.wait([this] { return m_done.load(std::memory_order_acquire); });
    settle();
  }

  /// @brief Waits for no longer than @p timeout.
  /// @return Whether it happened.
  [[nodiscard]] bool wait_for(const klib::Duration timeout) noexcept {
    const bool done =
        m_waiters.wait_for(timeout, [this] { return m_done.load(std::memory_order_acquire); });

    if (done) {
      settle();
    }

    return done;
  }

  /// @brief Says that it happened, and wakes everyone. Safe from interrupt handlers.
  void complete_all() noexcept {
    (void)m_waiters.wake_all_after([this] { m_done.store(true, std::memory_order_release); });
  }

  /// @brief Whether it happened.
  [[nodiscard]] bool is_done() const noexcept { return m_done.load(std::memory_order_acquire); }

  /// @brief Makes it ready to be used again.
  /// @pre Nothing is waiting or about to complete it.
  void reset() noexcept { m_done.store(false, std::memory_order_relaxed); }
};

/// @brief A Completion that takes several things to happen.
class Latch {
  std::atomic<std::ptrdiff_t> m_left;
  Completion m_done{};

 public:
  /// @param count How many times count_down() has to be called. 0 means it is open from the start.
  explicit Latch(const std::ptrdiff_t count) noexcept : m_left(count) {
    if (count == 0) {
      m_done.complete_all();
    }
  }

  Latch(const Latch&) = delete;
  Latch& operator=(const Latch&) = delete;

  /// @brief Says that @p count more things happened. Safe from interrupt handlers.
  void count_down(const std::ptrdiff_t count = 1) noexcept {
    if (m_left.fetch_sub(count, std::memory_order_acq_rel) == count) {
      m_done.complete_all();
    }
  }

  /// @brief Waits until everything happened.
  /// @pre The calling thread can sleep: see hw::percpu::can_sleep().
  void wait() noexcept { m_done.wait(); }

  /// @brief Waits for no longer than @p timeout.
  /// @return Whether everything happened.
  [[nodiscard]] bool wait_for(const klib::Duration timeout) noexcept {
    return m_done.wait_for(timeout);
  }

  /// @brief Whether everything happened.
  [[nodiscard]] bool is_done() const noexcept { return m_done.is_done(); }
};
}  // namespace kernel::sched
