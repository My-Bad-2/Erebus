#pragma once

#include <atomic>
#include <cstdint>
#include <new>

#include "hal/hal.hpp"
#include "hal/patch.h"

namespace kernel::utils {
namespace detail {
constexpr std::uint32_t BASE_PAUSE_LOOPS = 64;
constexpr std::uint32_t BASE_WAIT_TSC = 2000;

[[gnu::always_inline]] inline void wait_monitor(std::atomic<std::uint32_t> *addr, std::uint32_t curr) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(hw::detail::use_waitpkg)) {
    hw::umonitor(addr);
    if (addr->load(std::memory_order_relaxed) == curr) {
      hw::umwait(0, hw::read_tsc() + BASE_WAIT_TSC);
    }
  } else if (EREBUS_STATIC_BRANCH_UNLIKELY(hw::detail::use_moniterx)) {
    hw::monitorx(addr, 0, 0);
    if (addr->load(std::memory_order_relaxed) == curr) {
      hw::mwaitx(0, 0, BASE_WAIT_TSC);
    }
  } else {
    hw::pause();
  }
}

[[gnu::always_inline]] inline void wait_time(std::uint32_t distance) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(hw::detail::use_waitpkg)) {
    std::uint64_t deadline = hw::read_tsc() + (distance * BASE_WAIT_TSC);
    hw::tpause(0, deadline);
  } else {
    std::uint32_t delay = distance * BASE_PAUSE_LOOPS;
    while (delay--) {
      hw::pause();
    }
  }
}
} // namespace detail

class TicketSpinlock {
  alignas(std::hardware_destructive_interference_size) std::atomic<std::uint32_t> m_next_ticket{0};
  alignas(std::hardware_destructive_interference_size) std::atomic<std::uint32_t> m_now_serving{0};

public:
  constexpr TicketSpinlock() noexcept = default;

  TicketSpinlock(const TicketSpinlock &) = delete;
  TicketSpinlock &operator=(const TicketSpinlock &) = delete;

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
  RWLock(const RWLock &) = delete;
  RWLock &operator=(const RWLock &) = delete;

  void read_lock() noexcept;
  void read_unlock() noexcept;

  void write_lock() noexcept;
  void write_unlock() noexcept;
};

struct alignas(std::hardware_destructive_interference_size) CLHNode {
  std::atomic<std::uint32_t> locked{0};
};

class CLHQueue {
  std::atomic<CLHNode *> m_tail;
  CLHNode m_dummy_node{};

public:
  constexpr CLHQueue() noexcept : m_tail(&m_dummy_node) {}

  void wait_in_queue(CLHNode *my_node, CLHNode *&my_pred) noexcept;
  static void release(CLHNode *my_node) noexcept;
};

class alignas(std::hardware_destructive_interference_size) QSpinlock {
  std::atomic<std::uint32_t> m_val{0};
  alignas(std::hardware_destructive_interference_size) CLHQueue m_clh_queue;

  [[gnu::noinline]] void slow_path_lock() noexcept;

public:
  constexpr QSpinlock() noexcept = default;
  QSpinlock(const QSpinlock &) = delete;
  QSpinlock &operator=(const QSpinlock &) = delete;

  void lock() noexcept;
  void unlock() noexcept;
};

template <typename T>
concept BasicLockable = requires(T a) {
  { a.lock() } noexcept -> std::same_as<void>;
  { a.unlock() } noexcept -> std::same_as<void>;
};

template <typename T>
concept SharedLockable = requires(T a) {
  { a.read_lock() } noexcept -> std::same_as<void>;
  { a.read_unlock() } noexcept -> std::same_as<void>;
};

template <typename T>
concept ExclusiveLockable = requires(T a) {
  { a.write_lock() } noexcept -> std::same_as<void>;
  { a.write_unlock() } noexcept -> std::same_as<void>;
};

template <BasicLockable LockType> class [[nodiscard]] NakedGuard {
  LockType &m_lock;

public:
  explicit NakedGuard(LockType &lock) noexcept : m_lock(lock) { m_lock.lock(); }

  ~NakedGuard() noexcept { m_lock.unlock(); }

  NakedGuard(const NakedGuard &) = delete;
  NakedGuard &operator=(const NakedGuard &) = delete;
};

template <BasicLockable LockType> class [[nodiscard]] PreemptGuard {
  LockType &m_lock;

public:
  explicit PreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    // disable preemption
    m_lock.lock();
  }

  ~PreemptGuard() noexcept {
    m_lock.unlock();
    // enable preemption
  }

  PreemptGuard(const PreemptGuard &) = delete;
  PreemptGuard &operator=(const PreemptGuard &) = delete;
};

template <BasicLockable LockType> class [[nodiscard]] IrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit IrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    // Disable preemption
    m_lock.lock();
  }

  ~IrqSaveGuard() noexcept {
    m_lock.unlock();

    // enable preemption
    hw::irq::write_flags(m_saved_flags);
  }

  IrqSaveGuard(const IrqSaveGuard &) = delete;
  IrqSaveGuard &operator=(const IrqSaveGuard &) = delete;
};

template <SharedLockable LockType> class [[nodiscard]] SharedPreemptGuard {
  LockType &m_lock;

public:
  explicit SharedPreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    // disable preemption
    m_lock.read_lock();
  }

  ~SharedPreemptGuard() noexcept {
    m_lock.read_unlock();
    // enable preemption
  }

  SharedPreemptGuard(const SharedPreemptGuard &) = delete;
  SharedPreemptGuard &operator=(const SharedPreemptGuard &) = delete;
};

template <SharedLockable LockType> class [[nodiscard]] SharedIrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit SharedIrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    // disable preemption
    m_lock.read_lock();
  }

  ~SharedIrqSaveGuard() noexcept {
    m_lock.read_unlock();
    // enable preemption
    hw::irq::write_flags(m_saved_flags);
  }

  SharedIrqSaveGuard(const SharedIrqSaveGuard &) = delete;
  SharedIrqSaveGuard &operator=(const SharedIrqSaveGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusiveNakedGuard {
  LockType &m_lock;

public:
  explicit ExclusiveNakedGuard(LockType &lock) noexcept : m_lock(lock) { m_lock.write_lock(); }

  ~ExclusiveNakedGuard() noexcept { m_lock.write_unlock(); }

  ExclusiveNakedGuard(const ExclusiveNakedGuard &) = delete;
  ExclusiveNakedGuard &operator=(const ExclusiveNakedGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusivePreemptGuard {
  LockType &m_lock;

public:
  explicit ExclusivePreemptGuard(LockType &lock) noexcept : m_lock(lock) {
    // disable preemption
    m_lock.write_lock();
  }

  ~ExclusivePreemptGuard() noexcept {
    m_lock.write_unlock();
    // enable preemption
  }

  ExclusivePreemptGuard(const ExclusivePreemptGuard &) = delete;
  ExclusivePreemptGuard &operator=(const ExclusivePreemptGuard &) = delete;
};

template <ExclusiveLockable LockType> class [[nodiscard]] ExclusiveIrqSaveGuard {
  LockType &m_lock;
  std::uint64_t m_saved_flags;

public:
  explicit ExclusiveIrqSaveGuard(LockType &lock) noexcept : m_lock(lock) {
    m_saved_flags = hw::irq::read_flags();
    hw::irq::disable();

    // disable preemption
    m_lock.write_lock();
  }

  ~ExclusiveIrqSaveGuard() noexcept {
    m_lock.write_unlock();
    // enable preemption
    hw::irq::write_flags(m_saved_flags);
  }

  ExclusiveIrqSaveGuard(const ExclusiveIrqSaveGuard &) = delete;
  ExclusiveIrqSaveGuard &operator=(const ExclusiveIrqSaveGuard &) = delete;
};
} // namespace kernel::utils