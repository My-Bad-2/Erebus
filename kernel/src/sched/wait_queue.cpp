#include "sched/wait_queue.hpp"

#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "utils/assert.hpp"

namespace kernel::sched {
void WaitQueue::link_locked(Waiter& waiter) noexcept {
  waiter.m_prev = m_tail;
  waiter.m_next = nullptr;

  if (m_tail != nullptr) {
    m_tail->m_next = &waiter;
  } else {
    m_head = &waiter;
  }

  m_tail = &waiter;
  waiter.m_queued.store(true, std::memory_order_relaxed);
  m_count.store(m_count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void WaitQueue::unlink_locked(Waiter& waiter) noexcept {
  if (waiter.m_prev != nullptr) {
    waiter.m_prev->m_next = waiter.m_next;
  } else {
    m_head = waiter.m_next;
  }

  if (waiter.m_next != nullptr) {
    waiter.m_next->m_prev = waiter.m_prev;
  } else {
    m_tail = waiter.m_prev;
  }

  m_count.store(m_count.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

std::uint32_t WaitQueue::collect_locked(WakeList& wakes, const std::uint32_t max) noexcept {
  std::uint32_t collected = 0;

  while (collected < max && m_head != nullptr) {
    Waiter& waiter = *m_head;

    wakes.add(*waiter.m_thread);
    unlink_locked(waiter);

    // Last. Its owner can walk away from the waiter the moment it sees this.
    waiter.m_queued.store(false, std::memory_order_release);
    ++collected;
  }

  return collected;
}

void WaitQueue::prepare(Waiter& waiter) noexcept {
  KASSERT(hw::percpu::can_sleep() && hw::irq::is_enabled());

  {
    utils::IrqSaveGuard guard{m_lock};

    // Before it can be found. A waker that takes it off the queue must see a blocked thread.
    Scheduler::prepare_to_block();

    if (!waiter.m_queued.load(std::memory_order_relaxed)) {
      link_locked(waiter);
    }
  }

  // The other half is in wake(), which can skip the lock if it finds nobody here. Either that sees
  // the count that was just raised, or the condition about to be read sees what it changed.
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

bool WaitQueue::finish(Waiter& waiter) noexcept {
  Scheduler::cancel_block();

  if (!waiter.m_queued.load(std::memory_order_acquire)) {
    return true;
  }

  utils::IrqSaveGuard guard{m_lock};

  // A wake can have taken it off while this was on its way to the lock.
  if (!waiter.m_queued.load(std::memory_order_relaxed)) {
    return true;
  }

  unlink_locked(waiter);
  waiter.m_queued.store(false, std::memory_order_relaxed);
  return false;
}

std::uint32_t WaitQueue::wake(const std::uint32_t count) noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (m_count.load(std::memory_order_relaxed) == 0) {
    return 0;
  }

  WakeList wakes;
  std::uint32_t woken = 0;

  hw::percpu::preempt_disable();
  {
    utils::IrqSaveGuard guard{m_lock};
    woken = collect_locked(wakes, count);
  }

  wakes.wake_all();
  hw::percpu::preempt_enable();
  return woken;
}
}  // namespace kernel::sched
