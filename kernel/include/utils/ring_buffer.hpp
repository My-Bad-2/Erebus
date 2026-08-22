#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "hal/hal.hpp"

namespace kernel::utils {
enum class DequeError : uint8_t { Empty, Full };

template <typename T>
concept RingQueable = std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>;

template <RingQueable T, size_t Capacity, bool MultiConsumer, bool PadSlots> class RingBuffer {
  static_assert(Capacity >= 2 && std::has_single_bit(Capacity), "Capacity must be a power of 2 and >= 2");

public:
  static constexpr std::size_t Mask = Capacity - 1;

  struct WriteTicket {
    T *ptr{nullptr};
    std::size_t head{0};
    [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }
  };

private:
  struct alignas(PadSlots ? std::hardware_destructive_interference_size : alignof(T)) Slot {
    std::atomic<size_t> sequence;
    union {
      T data;
    };

    Slot() noexcept : sequence(0), data() {}
    ~Slot() noexcept {}
  };

  alignas(std::hardware_destructive_interference_size) std::atomic<size_t> m_head{0};
  alignas(std::hardware_destructive_interference_size) std::atomic<size_t> m_tail{0};
  alignas(std::hardware_destructive_interference_size) Slot m_slots[Capacity];

public:
  constexpr RingBuffer() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      m_slots[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~RingBuffer() noexcept {
    T item;
    while (try_dequeue(item)) {
    }
  }

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;
  RingBuffer(RingBuffer &&) = delete;
  RingBuffer &operator=(RingBuffer &&) = delete;

  template <typename... Args> [[nodiscard]] bool try_emplace(Args &&...args) noexcept {
    std::size_t head = m_head.load(std::memory_order_relaxed);
    Slot *slot = nullptr;

    while (true) {
      slot = &m_slots[head & Mask];
      const std::size_t seq = slot->sequence.load(std::memory_order_relaxed);
      const auto diff = static_cast<std::intptr_t>(seq - head);

      if (diff == 0) {
        if (m_head.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        head = m_head.load(std::memory_order_relaxed);
      }

      hw::cpu_relax();
    }

    std::construct_at(&slot->data, std::forward<Args>(args)...);
    slot->sequence.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_enqueue(const T &item) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    return try_emplace(item);
  }

  [[nodiscard]] bool try_enqueue(T &&item) noexcept
    requires std::is_nothrow_move_constructible_v<T>
  {
    return try_emplace(std::move(item));
  }

  [[nodiscard]] WriteTicket try_reserve() noexcept {
    std::size_t head = m_head.load(std::memory_order_relaxed);
    Slot *slot = nullptr;

    for (;;) {
      slot = &m_slots[head & Mask];
      const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq - head);

      if (diff == 0) {
        if (m_head.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
          return {.ptr = reinterpret_cast<T *>(&slot->data), .head = head};
        }
      } else if (diff < 0) {
        return {.ptr = nullptr, .head = 0}; // Buffer full
      } else {
        head = m_head.load(std::memory_order_relaxed);
      }

      hw::cpu_relax();
    }
  }

  void commit(const WriteTicket &ticket) noexcept {
    Slot *slot = &m_slots[ticket.head & Mask];
    slot->sequence.store(ticket.head + 1, std::memory_order_release);
  }

  [[nodiscard]] bool try_dequeue(T &out) noexcept {
    std::size_t tail = m_tail.load(std::memory_order_relaxed);
    Slot *slot = nullptr;

    for (;;) {
      slot = &m_slots[tail & Mask];
      const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq - (tail + 1));

      if (diff == 0) {
        if constexpr (MultiConsumer) {
          if (!m_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed)) {
            hw::cpu_relax();
            continue;
          }
        }

        out = std::move(slot->data);
        std::destroy_at(&slot->data);

        slot->sequence.store(tail + Capacity, std::memory_order_release);

        if constexpr (!MultiConsumer) {
          m_tail.store(tail + 1, std::memory_order_relaxed);
        }

        return true;
      }

      if (diff < 0) {
        return false; // Buffer empty
      }

      tail = m_tail.load(std::memory_order_relaxed);

      if constexpr (MultiConsumer) {
        hw::cpu_relax();
      }
    }
  }

  std::size_t enqueue_bulk(std::span<const T> items) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    std::size_t written = 0;
    for (const auto &item : items) {
      if (!try_enqueue(item)) {
        break;
      }

      ++written;
    }

    return written;
  }

  std::size_t dequeue_bulk(std::span<T> out_items) noexcept {
    std::size_t read = 0;
    for (auto &dest : out_items) {
      if (!try_dequeue(dest)) {
        break;
      }

      ++read;
    }

    return read;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t head = m_head.load(std::memory_order_relaxed);
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    return (head >= tail) ? (head - tail) : 0;
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
  [[nodiscard]] bool full_approx() const noexcept { return size_approx() >= Capacity; }
};

template <RingQueable T, std::size_t Capacity>
  requires(std::has_single_bit(Capacity) && Capacity >= 2)
class RingDeque {
protected:
  static constexpr std::size_t Mask = Capacity - 1;

  alignas(std::hardware_destructive_interference_size) std::size_t m_head{0};
  alignas(std::hardware_destructive_interference_size) std::size_t m_tail{0};
  alignas(std::hardware_destructive_interference_size) std::byte m_storage[Capacity * sizeof(T)];

  [[nodiscard]] constexpr T *ptr(const std::size_t index) noexcept {
    return reinterpret_cast<T *>(&m_storage[(index & Mask) * sizeof(T)]);
  }

public:
  constexpr RingDeque() noexcept = default;
  constexpr ~RingDeque() noexcept { clear(); }

  RingDeque(const RingDeque &) = delete;
  RingDeque &operator=(const RingDeque &) = delete;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return m_tail - m_head; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }
  [[nodiscard]] constexpr bool empty() const noexcept { return m_head == m_tail; }
  [[nodiscard]] constexpr bool full() const noexcept { return size() == Capacity; }

  template <typename... Args> constexpr std::expected<void, DequeError> emplace_back(Args &&...args) noexcept {
    if (full()) [[unlikely]] {
      return std::unexpected(DequeError::Full);
    }

    std::construct_at(ptr(m_tail++), std::forward<Args>(args)...);
    return {};
  }

  template <typename... Args> constexpr std::expected<void, DequeError> emplace_front(Args &&...args) noexcept {
    if (full()) [[unlikely]] {
      return std::unexpected(DequeError::Full);
    }

    const std::size_t new_head = m_head - 1;
    std::construct_at(ptr(new_head), std::forward<Args>(args)...);
    m_head = new_head;
    return {};
  }

  constexpr std::expected<T, DequeError> pop_back() noexcept {
    if (empty()) [[unlikely]] {
      return std::unexpected(DequeError::Empty);
    }

    T *p = ptr(--m_tail);
    T val = std::move(*p);
    std::destroy_at(p);
    return val;
  }

  constexpr std::expected<T, DequeError> pop_front() noexcept {
    if (empty()) [[unlikely]] {
      return std::unexpected(DequeError::Empty);
    }

    T *p = ptr(m_head);
    T val = std::move(*p);
    std::destroy_at(p);
    ++m_head;
    return val;
  }

  constexpr std::size_t push_back_bulk(std::span<const T> items) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    const std::size_t available = Capacity - size();
    const std::size_t to_write = (items.size() < available) ? items.size() : available;

    for (std::size_t i = 0; i < to_write; ++i) {
      std::construct_at(ptr(m_tail++), items[i]);
    }

    return to_write;
  }

  constexpr std::size_t pop_front_bulk(std::span<T> out) noexcept {
    const std::size_t available = size();
    const std::size_t to_read = (out.size() < available) ? out.size() : available;

    for (std::size_t i = 0; i < to_read; ++i) {
      T *p = ptr(m_head);
      out[i] = std::move(*p);
      std::destroy_at(p);
      ++m_head;
    }

    return to_read;
  }

  constexpr void clear() noexcept {
    while (!empty()) {
      std::destroy_at(ptr(m_head++));
    }
  }
};

template <RingQueable T, size_t Capacity, bool PadSlots = true>
using MpscRingBuffer = RingBuffer<T, Capacity, false, PadSlots>;

template <RingQueable T, size_t Capacity, bool PadSlots = true>
using MpmcRingBuffer = RingBuffer<T, Capacity, true, PadSlots>;
} // namespace kernel::utils