#include "sched/runqueue.hpp"

#include <utility>

#include "sched/numa_domain.hpp"
#include "sched/thread.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"

namespace kernel::sched {
std::uint32_t RunQueue::rt_level(const Thread& thread) noexcept {
  switch (thread.priority_class()) {
    case PriorityClass::TestRunner:
      return 0;
    case PriorityClass::RealTime:
      return 1;
    default:
      return RT_LEVELS;
  }
}

void RunQueue::initialize(const std::uint32_t cpu_id, NumaDomain* domain, const std::uint32_t slot,
                          Thread* idle_thread) noexcept {
  m_cpu_id = cpu_id;
  m_domain = domain;
  m_slot = slot;
  m_idle_thread = idle_thread;
  m_current.store(idle_thread, std::memory_order_release);

  // Any odd multiple of the golden ratio will do, as long as CPUs do not all start out the same.
  m_rng_state = (std::uint64_t{cpu_id} + 1) * 0x9E37'79B9'7F4A'7C15ull;
}

std::uint32_t RunQueue::highest_rt_level() const noexcept {
  for (std::uint32_t level = 0; level < RT_LEVELS; ++level) {
    if (!m_realtime[level].list.empty()) {
      return level;
    }
  }

  return RT_LEVELS;
}

void RunQueue::account_added(const Thread& thread) noexcept {
  m_waiting.store(m_waiting.load(std::memory_order_relaxed) + 1, std::memory_order_release);

  if (thread.is_pinned()) {
    return;
  }

  const std::uint32_t stealable = m_stealable.load(std::memory_order_relaxed);
  m_stealable.store(stealable + 1, std::memory_order_release);
  if (stealable == 0 && m_domain != nullptr) {
    m_domain->set_overloaded(m_slot, true);
  }
}

void RunQueue::account_removed(const Thread& thread) noexcept {
  m_waiting.store(m_waiting.load(std::memory_order_relaxed) - 1, std::memory_order_release);

  if (thread.is_pinned()) {
    return;
  }

  const std::uint32_t stealable = m_stealable.load(std::memory_order_relaxed);
  m_stealable.store(stealable - 1, std::memory_order_release);
  if (stealable == 1 && m_domain != nullptr) {
    m_domain->set_overloaded(m_slot, false);
  }
}

void RunQueue::push_realtime(Thread& thread, const bool at_front) noexcept {
  RtFifo& fifo = m_realtime[rt_level(thread)];

  if (at_front || fifo.list.empty()) {
    const bool was_empty = fifo.list.empty();
    fifo.list.push_front(thread);
    if (was_empty) {
      fifo.tail = &thread;
    }

    return;
  }

  fifo.list.insert_after(fifo.list.iterator_to(*fifo.tail), thread);
  fifo.tail = &thread;
}

Thread* RunQueue::pop_realtime(const std::uint32_t level) noexcept {
  RtFifo& fifo = m_realtime[level];

  Thread* thread = fifo.list.pop_front();
  if (fifo.list.empty()) {
    fifo.tail = nullptr;
  }

  return thread;
}

bool RunQueue::remove_realtime(Thread& thread) noexcept {
  RtFifo& fifo = m_realtime[rt_level(thread)];

  auto before = fifo.list.before_begin();
  for (auto it = fifo.list.begin(); it != fifo.list.end(); before = it, ++it) {
    if (&*it != &thread) {
      continue;
    }

    fifo.list.erase_after(before);
    if (fifo.tail == &thread) {
      fifo.tail = before == fifo.list.before_begin() ? nullptr : &*before;
    }

    return true;
  }

  return false;
}

void RunQueue::update_clock_locked(const std::uint64_t now_ns) noexcept {
  // TSCs are only close to each other, and a queue is also updated by other CPUs.
  if (now_ns > m_charged_until_ns) {
    (void)m_fair.charge(now_ns - m_charged_until_ns);
    m_charged_until_ns = now_ns;
  }
}

bool RunQueue::enqueue_locked(Thread& thread) noexcept {
  KASSERT(!thread.is_idle());

  thread.mark_queued();

  const std::uint32_t level = rt_level(thread);
  if (level < RT_LEVELS) {
    push_realtime(thread, false);
  } else {
    m_fair.enqueue(thread);
  }

  account_added(thread);

  const Thread* running = current();
  if (running == m_idle_thread) {
    return true;
  }

  const std::uint32_t running_level = rt_level(*running);
  if (level < RT_LEVELS || running_level < RT_LEVELS) {
    return level < running_level;
  }

  return m_fair.preempts_current(thread);
}

bool RunQueue::remove_locked(Thread& thread) noexcept {
  if (!thread.on_runqueue()) {
    return false;
  }

  if (rt_level(thread) < RT_LEVELS) {
    if (!remove_realtime(thread)) {
      return false;
    }
  } else {
    if (!thread.is_queued()) {
      return false;
    }

    m_fair.dequeue(thread);
  }

  thread.set_on_runqueue(false);
  account_removed(thread);
  return true;
}

void RunQueue::requeue_current_fair() noexcept {
  auto& thread = static_cast<Thread&>(*m_fair.current());

  m_fair.requeue_current();
  thread.mark_queued();
  account_added(thread);
}

void RunQueue::requeue_realtime(Thread& thread, const bool at_front) noexcept {
  push_realtime(thread, at_front);
  thread.mark_queued();
  account_added(thread);
}

Thread* RunQueue::switch_in(Thread& next, const Thread& prev) noexcept {
  next.set_on_runqueue(false);
  m_current.store(&next, std::memory_order_release);
  m_running_realtime.store(rt_level(next) < RT_LEVELS, std::memory_order_release);
  ++m_switches;

  if (prev.is_idle() && m_domain != nullptr) {
    m_domain->mark_busy(m_slot);
  }

  return &next;
}

Thread* RunQueue::pick_next_locked(Thread& prev, const PickMode mode) noexcept {
  const bool preempted = mode == PickMode::Preempted;
  const bool runnable = !prev.is_idle() && (preempted || prev.state() == ThreadState::Running);
  const bool prev_fair = m_fair.current() == &prev;
  const std::uint32_t prev_level = rt_level(prev);
  const bool prev_rt = runnable && prev_level < RT_LEVELS;

  if (!prev.is_idle() && !runnable) {
    if (prev_fair) {
      m_fair.retire_current();
    }

    if (prev.state() == ThreadState::Dead) {
      m_dead.push_front(prev);
    }
  } else if (prev_fair && mode == PickMode::Yield) {
    m_fair.yield_current();
  }

  if (const std::uint32_t top = highest_rt_level(); top < RT_LEVELS) {
    const bool outranked = top < prev_level || (top == prev_level && mode == PickMode::Yield);
    if (prev_rt && !outranked) {
      return &prev;
    }

    if (prev_rt) {
      // A thread that lost the CPU keeps its place in line; one that yielded goes to the end.
      requeue_realtime(prev, preempted);
    } else if (runnable && prev_fair) {
      requeue_current_fair();
    }

    Thread* next = pop_realtime(top);
    account_removed(*next);
    return switch_in(*next, prev);
  }

  if (prev_rt) {
    return &prev;
  }

  Entity* chosen = m_fair.pick();
  if (chosen == nullptr) {
    return &prev == m_idle_thread ? &prev : switch_in(*m_idle_thread, prev);
  }

  if (chosen == m_fair.current()) {
    return &prev;
  }

  if (m_fair.current() != nullptr) {
    requeue_current_fair();
  }

  auto& next = static_cast<Thread&>(*chosen);
  m_fair.set_current(next);
  account_removed(next);
  return switch_in(next, prev);
}

bool RunQueue::should_switch_locked() const noexcept {
  const Thread* running = current();
  if (running == m_idle_thread) {
    return waiting() != 0;
  }

  const std::uint32_t running_level = rt_level(*running);
  if (const std::uint32_t top = highest_rt_level(); top < RT_LEVELS) {
    return top < running_level;
  }

  return running_level >= RT_LEVELS && m_fair.pick() != m_fair.current();
}

bool RunQueue::wants_timer_locked() const noexcept {
  return m_fair.current() != nullptr && waiting() != 0;
}

// Waiting real-time threads first, as they are the ones who lose the most by waiting. Of the fair
// ones, the latest deadline: it has the longest to wait, and its cache is the coldest.
Thread* RunQueue::find_stealable() noexcept {
  std::uint32_t budget = STEAL_SCAN_LIMIT;

  for (std::uint32_t level = 0; level < RT_LEVELS; ++level) {
    for (Thread& thread : m_realtime[level].list) {
      if (budget-- == 0) {
        return nullptr;
      }

      if (!thread.is_pinned()) {
        return &thread;
      }
    }
  }

  return static_cast<Thread*>(m_fair.find_latest(
      [](const Entity& entity) { return !static_cast<const Thread&>(entity).is_pinned(); },
      budget));
}

Thread* RunQueue::try_steal(const std::uint32_t enough) noexcept {
  if (spare() < enough || !m_lock.try_lock()) {
    return nullptr;
  }

  Thread* thread = nullptr;
  if (spare() >= enough) {
    thread = find_stealable();
    if (thread != nullptr) {
      (void)remove_locked(*thread);
    }
  }

  m_lock.unlock();
  return thread;
}

std::size_t RunQueue::drain_dead() noexcept {
  if (m_dead.empty()) {
    return 0;
  }

  utils::SList<Thread, ReapQueueTag> dead;
  {
    utils::IrqSaveGuard guard{*this};
    dead = std::move(m_dead);
  }

  std::size_t reaped = 0;
  while (!dead.empty()) {
    reaped += Thread::destroy(dead.pop_front()).has_value();
  }

  return reaped;
}

void RunQueue::publish_idle() noexcept {
  if (m_domain != nullptr) {
    m_domain->mark_idle(m_slot);
  }
}

std::uint64_t RunQueue::next_random() noexcept {
  m_rng_state ^= m_rng_state << 13;
  m_rng_state ^= m_rng_state >> 7;
  m_rng_state ^= m_rng_state << 17;
  return m_rng_state;
}
}  // namespace kernel::sched
