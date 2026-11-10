/// @file io_ring.hpp
/// @brief A submission queue and a completion queue, with doorbells that only ring for a side
///        that is asleep.
///
/// A client puts requests in the submission queue and a driver answers in the completion queue,
/// both lock-free, so neither goes through the kernel while the other is busy. A doorbell, a
/// signal on a notification, only rings for a side that said it is going to sleep. That side says
/// so, then looks at its queue once more before it waits; the other side looks at the flag after
/// it put something in. With a full fence on both sides in between, one of them always sees the
/// other, so nothing is left in a queue with its consumer asleep.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>

#include "core/errors.hpp"
#include "core/ipc/notification.hpp"
#include "utils/ring_buffer.hpp"

namespace kernel::core::ipc {
template <utils::RingQueable Req, utils::RingQueable Resp, std::size_t Capacity = 64>
struct alignas(std::hardware_destructive_interference_size) SharedIoRing {
  utils::MpscRingBuffer<Req, Capacity, false> sq{};
  utils::MpscRingBuffer<Resp, Capacity, false> cq{};

  alignas(std::hardware_destructive_interference_size) std::atomic<bool> driver_needs_wakeup{false};
  alignas(std::hardware_destructive_interference_size) std::atomic<bool> client_needs_wakeup{false};

  constexpr SharedIoRing() noexcept = default;

  /// @brief Client: queues @p request, and rings @p doorbell if the driver sleeps.
  /// @return false if the queue is full.
  [[nodiscard]] bool submit(const Req& request, Notification* const doorbell = nullptr,
                            const std::uint64_t badge = 1) noexcept {
    return put(sq, request, driver_needs_wakeup, doorbell, badge);
  }

  /// @brief Driver: queues @p response, and rings @p doorbell if the client sleeps.
  [[nodiscard]] bool complete(const Resp& response, Notification* const doorbell = nullptr,
                              const std::uint64_t badge = 1) noexcept {
    return put(cq, response, client_needs_wakeup, doorbell, badge);
  }

  /// @brief Driver: the next request, waiting on @p doorbell for one if there is none.
  [[nodiscard]] std::expected<void, Error> next_request(Req& out, Notification& doorbell) noexcept {
    return take(sq, out, driver_needs_wakeup, doorbell);
  }

  /// @brief Client: the next response, waiting on @p doorbell for one if there is none.
  [[nodiscard]] std::expected<void, Error> next_response(Resp& out,
                                                         Notification& doorbell) noexcept {
    return take(cq, out, client_needs_wakeup, doorbell);
  }

 private:
  template <typename Queue, typename T>
  [[nodiscard]] static bool put(Queue& queue, const T& item, std::atomic<bool>& sleeper,
                                Notification* const doorbell, const std::uint64_t badge) noexcept {
    if (!queue.try_enqueue(item)) [[unlikely]] {
      return false;
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (doorbell != nullptr && sleeper.load(std::memory_order_relaxed)) {
      doorbell->signal(badge);
    }

    return true;
  }

  template <typename Queue, typename T>
  [[nodiscard]] static std::expected<void, Error> take(Queue& queue, T& out,
                                                       std::atomic<bool>& sleeper,
                                                       Notification& doorbell) noexcept {
    while (!queue.try_dequeue(out)) {
      sleeper.store(true, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);

      // Once more, now that the other side would ring: what it put in before it looked is here.
      if (queue.try_dequeue(out)) {
        sleeper.store(false, std::memory_order_relaxed);
        return {};
      }

      const auto rung = doorbell.wait();
      sleeper.store(false, std::memory_order_relaxed);
      if (!rung) [[unlikely]] {
        return std::unexpected(rung.error());
      }
    }

    return {};
  }
};
}  // namespace kernel::core::ipc
