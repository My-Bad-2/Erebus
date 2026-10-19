#pragma once

#include <atomic>
#include <cstdint>
#include <new>

#include "patch/static_call.hpp"

namespace kernel::utils {
namespace detail {
DECLARE_STATIC_CALL(void, wait_monitor, std::atomic<std::uint32_t>*, std::uint32_t);
DECLARE_STATIC_CALL(void, wait_time, std::uint32_t);
}  // namespace detail

class TicketSpinlock {
  alignas(std::hardware_destructive_interference_size) std::atomic<std::uint32_t> m_next_ticket{0};
  alignas(std::hardware_destructive_interference_size) std::atomic<std::uint32_t> m_now_serving{0};

 public:
  constexpr TicketSpinlock() noexcept = default;

  TicketSpinlock(const TicketSpinlock&) = delete;
  TicketSpinlock& operator=(const TicketSpinlock&) = delete;

  void lock() noexcept;
  void unlock() noexcept;
  [[nodiscard]] bool try_lock() noexcept;
};

union RWState {
  std::uint32_t raw;
  struct {
    std::uint8_t active_readers;
    std::uint8_t serving;
    std::uint16_t write_ticket;
  } parts;
};

class alignas(std::hardware_destructive_interference_size) RWLock {
  alignas(std::hardware_destructive_interference_size) TicketSpinlock m_write_queue;
  alignas(std::hardware_destructive_interference_size) std::atomic<std::uint32_t> m_rw_state{0};

 public:
  constexpr RWLock() noexcept = default;
  RWLock(const RWLock&) = delete;
  RWLock& operator=(const RWLock&) = delete;

  void read_lock() noexcept;
  void read_unlock() noexcept;

  void write_lock() noexcept;
  void write_unlock() noexcept;
};

struct alignas(std::hardware_destructive_interference_size) CLHNode {
  std::atomic<std::uint32_t> locked{0};
};

class CLHQueue {
  std::atomic<CLHNode*> m_tail;
  CLHNode m_dummy_node{};

 public:
  constexpr CLHQueue() noexcept : m_tail(&m_dummy_node) {}

  void wait_in_queue() noexcept;
  void release() noexcept;
};

class alignas(std::hardware_destructive_interference_size) QSpinlock {
  std::atomic<std::uint32_t> m_val{0};
  alignas(std::hardware_destructive_interference_size) CLHQueue m_clh_queue;

  [[gnu::cold]] void slow_path_lock() noexcept;

 public:
  constexpr QSpinlock() noexcept = default;
  QSpinlock(const QSpinlock&) = delete;
  QSpinlock& operator=(const QSpinlock&) = delete;

  void lock() noexcept;
  void unlock() noexcept;
};
}  // namespace kernel::utils