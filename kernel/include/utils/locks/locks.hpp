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

/// @brief FIFO spinlock built from two counters: the next ticket and the one being served.
///
/// Both counters share a cache line. Splitting them makes every uncontended lock() touch two
/// lines for no gain, since every waiter watches `m_now_serving` anyway. The waiter right behind
/// the owner sleeps on the counter with MONITOR/MWAIT-style waits where available; the others
/// back off in proportion to their distance from the front.
class TicketSpinlock {
  std::atomic<std::uint32_t> m_now_serving{0};
  std::atomic<std::uint32_t> m_next_ticket{0};

 public:
  constexpr TicketSpinlock() noexcept = default;

  TicketSpinlock(const TicketSpinlock&) = delete;
  TicketSpinlock& operator=(const TicketSpinlock&) = delete;

  void lock() noexcept;
  void unlock() noexcept;
  [[nodiscard]] bool try_lock() noexcept;
};

/// @brief Writer-preferring reader/writer spinlock.
///
/// The state word counts readers from bit 8 up and has two writer bits below. A writer first
/// takes `m_write_queue`, so writers queue in FIFO order among themselves, then announces itself
/// and waits for the readers inside to leave. New readers back off while a writer waits, so a
/// stream of readers cannot starve writers. The flip side: a CPU that already holds the read
/// lock must not take it again, or it may wait for a writer that waits for it.
///
/// The object is small (12 bytes) and not over-aligned; put it next to what it protects.
class RWLock {
  std::atomic<std::uint32_t> m_rw_state{0};
  TicketSpinlock m_write_queue;

 public:
  constexpr RWLock() noexcept = default;
  RWLock(const RWLock&) = delete;
  RWLock& operator=(const RWLock&) = delete;

  void read_lock() noexcept;
  void read_unlock() noexcept;

  void write_lock() noexcept;
  void write_unlock() noexcept;
};

/// @brief A lock that does nothing, for data structures that are already protected by a lock
///        their owner holds. Satisfies every lock concept in guards.hpp.
struct NullLock {
  constexpr void lock() noexcept {}
  constexpr void unlock() noexcept {}
  [[nodiscard]] constexpr bool try_lock() noexcept { return true; }

  constexpr void read_lock() noexcept {}
  constexpr void read_unlock() noexcept {}
  constexpr void write_lock() noexcept {}
  constexpr void write_unlock() noexcept {}
};

/// @brief A CPU's place in a QSpinlock wait queue.
///
/// Each CPU owns QSpinlock::MAX_NESTING of these, one per context that can be waiting for a
/// lock at the same time (normal code, and whatever interrupts it). A node is only in use while
/// its CPU waits, never while it holds a lock, so holding any number of locks costs nothing.
struct alignas(16) McsNode {
  std::atomic<McsNode*> next{nullptr};
  std::atomic<std::uint32_t> locked{0};  ///< Set by the predecessor when we reach the front.
  std::uint32_t count{0};                ///< Node 0 only: how many of the CPU's nodes are taken.
};

/// @brief Queued spinlock: a 32-bit lock word with an MCS queue behind it.
///
/// This follows Linux's qspinlock, in the layout it uses for very large machines. The word holds
/// a locked byte, a pending bit and the tail of the wait queue, encoded as (CPU, node index):
///
///  - Uncontended, lock() and unlock() are a single atomic operation each.
///  - The first waiter does not queue. It sets the pending bit and spins on the word itself.
///  - Everyone after that appends their per-CPU node to the queue and spins on their *own* node,
///    so waiting CPUs do not bounce the lock's cache line between them. Only the CPU at the
///    front of the queue watches the lock word.
///
/// Waiting is FIFO. The lock is not recursive, and code that can run in interrupt context must
/// take it with interrupts disabled, as with any spinlock.
class QSpinlock {
  std::atomic<std::uint32_t> m_val{0};

  [[gnu::cold]] void slow_path_lock(std::uint32_t val) noexcept;
  void queue_and_lock() noexcept;

 public:
  /// Contexts per CPU that may wait for a QSpinlock at the same time (task, softirq, IRQ, NMI).
  static constexpr std::uint32_t MAX_NESTING = 4;
  /// The tail stores the CPU ID plus one in 21 bits.
  static constexpr std::uint32_t MAX_CPUS = (1u << 21) - 1;

  constexpr QSpinlock() noexcept = default;
  QSpinlock(const QSpinlock&) = delete;
  QSpinlock& operator=(const QSpinlock&) = delete;

  void lock() noexcept;
  void unlock() noexcept;
  [[nodiscard]] bool try_lock() noexcept;
};
}  // namespace kernel::utils