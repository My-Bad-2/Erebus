/// @file notification.hpp
/// @brief Asynchronous signals: a word of bits that signals set and a wait takes, all at once.
///
/// signal() never waits and works from anywhere but an NMI, interrupt handlers included, which is
/// how a device's interrupt gets to its driver (IrqHandler). A waiter gets every bit set since the
/// last one took them, and leaves the word clear: many signals before a wait make one wakeup.
///
/// A notification can be bound to one thread's ThreadIpcContext, so that the thread can wait for a
/// message and a signal at once: Endpoint::recv() comes back with the bits instead, as a message
/// with MsgFlags::IsAsyncNotification, if they come first.
///
/// When the last capability goes, threads waiting on it are woken with Error::InvalidState.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstdint>
#include <expected>

#include "core/cap.hpp"
#include "core/errors.hpp"
#include "sched/wait_queue.hpp"
#include "utils/lock.hpp"

namespace kernel::core::ipc {
class ThreadIpcContext;

/// @brief A word of signals. See the file comment.
class alignas(std::hardware_destructive_interference_size) Notification final
    : public TypedCapObject<Notification, CapType::Notification> {
 public:
  constexpr Notification() noexcept = default;
  ~Notification() noexcept;

  /// @brief Sets @p bits, and wakes a waiter, or the bound thread. Nothing for 0.
  void signal(std::uint64_t bits) noexcept;

  /// @brief Takes whatever bits are set, without waiting.
  [[nodiscard]] std::uint64_t poll() noexcept {
    return m_word.exchange(0, std::memory_order_acq_rel);
  }

  /// @brief Looks at the bits, leaving them set.
  [[nodiscard]] std::uint64_t peek() const noexcept {
    return m_word.load(std::memory_order_seq_cst);
  }

  /// @brief Waits for bits, and takes them. @pre The caller can sleep.
  /// @return InvalidState once the notification is closed.
  [[nodiscard]] std::expected<std::uint64_t, Error> wait() noexcept;

  /// @brief wait() for @p timeout at most. @return Timeout if nothing came.
  [[nodiscard]] std::expected<std::uint64_t, Error> wait_for(klib::Duration timeout) noexcept;

  /// @brief Binds to @p ctx, which holds a reference until unbind(). One thread at a time, and one
  ///        notification per thread. @return InvalidState if either is bound elsewhere.
  [[nodiscard]] std::expected<void, Error> bind(ThreadIpcContext& ctx) noexcept;

  /// @brief Undoes bind(), if @p ctx is the one bound.
  void unbind(ThreadIpcContext& ctx) noexcept;

  /// @brief The last capability went: waiters are woken, and nobody waits from now on.
  void on_unreachable() noexcept;

 private:
  std::atomic<std::uint64_t> m_word{0};
  std::atomic<bool> m_closed{false};
  sched::WaitQueue m_waiters{};

  utils::QSpinlock m_lock{};  ///< For the binding. Signals take it from interrupt handlers.
  ThreadIpcContext* m_bound{nullptr};
};
}  // namespace kernel::core::ipc
