#include "utils/lock.hpp"

#include "hal/percpu.hpp"

namespace kernel::utils {
namespace {
constexpr std::uint32_t QLOCK_LOCKED = 1 << 0;
constexpr std::uint32_t QLOCK_PENDING = 1 << 1;
constexpr std::uint32_t QLOCK_QUEUED = 1 << 2;
} // namespace

void CLHQueue::wait_in_queue() noexcept {
  CLHNode *my_node = hw::percpu::get_curr_node();
  my_node->locked.store(1, std::memory_order_relaxed);

  CLHNode *my_pred = m_tail.exchange(my_node, std::memory_order_acq_rel);
  hw::percpu::set_prev_node(my_pred);

  if (my_pred->locked.load(std::memory_order_acquire) == 1) {
    detail::wait_monitor(&my_pred->locked, 1);
  }
}

void CLHQueue::release() noexcept {
  CLHNode *my_node = hw::percpu::get_curr_node();
  my_node->locked.store(0, std::memory_order_release);
}

void QSpinlock::lock() noexcept {
  std::uint32_t expected = 0;
  if (m_val.compare_exchange_strong(expected, QLOCK_LOCKED, std::memory_order_acquire, std::memory_order_relaxed))
      [[likely]] {
    return; // Lock acquired
  }

  // If the lock is held, but nobody is pending and the queue is empty, we can take the pending slot.
  if ((expected & (QLOCK_PENDING | QLOCK_QUEUED)) == 0) {
    if (m_val.compare_exchange_strong(expected, expected | QLOCK_PENDING, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
      // We are the pending owner. Wait for the primary lock to clear.
      while (m_val.load(std::memory_order_relaxed) & QLOCK_LOCKED) {
        hw::pause();
      }

      // Take ownership over the lock.
      const std::uint32_t curr = m_val.load(std::memory_order_relaxed);
      m_val.store((curr & ~QLOCK_PENDING) | QLOCK_LOCKED, std::memory_order_acquire);
      return;
    }
  }

  slow_path_lock();
}

void QSpinlock::unlock() noexcept {
  const std::uint32_t prev = m_val.fetch_and(~QLOCK_LOCKED, std::memory_order_release);

  if (prev & QLOCK_QUEUED) {
    m_clh_queue.release();
    hw::percpu::set_curr_node(hw::percpu::get_prev_node());
  }
}

void QSpinlock::slow_path_lock() noexcept {
  // Again fetch the per-cpu state
  m_val.fetch_or(QLOCK_QUEUED, std::memory_order_relaxed);
  m_clh_queue.wait_in_queue();

  while (true) {
    const std::uint32_t curr = m_val.load(std::memory_order_acquire);

    if ((curr & (QLOCK_LOCKED | QLOCK_PENDING)) == 0) {
      m_val.fetch_or(QLOCK_LOCKED, std::memory_order_acquire);
      return;
    }

    hw::pause();
  }
}
} // namespace kernel::utils