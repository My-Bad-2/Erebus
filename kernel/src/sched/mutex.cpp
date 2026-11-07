#include "sched/mutex.hpp"

#include <algorithm>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/smp.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::sched {
namespace {
constexpr std::uint32_t MAX_SPINS = 1024;
static_assert(alignof(Thread) >= 4, "The flags of a Mutex live in the low bits of its owner");
}  // namespace

// The owner's memory may be gone by the time this looks, as it can unlock and exit at any point.
// So this does not touch it, and compares its address with what a CPU says it is running. If it
// moved to another CPU while holding the lock, that says no, and the waiter sleeps.
bool Mutex::spin_on_owner() noexcept {
  for (std::uint32_t spin = 0; spin < MAX_SPINS; ++spin) {
    const std::uintptr_t state = m_state.load(std::memory_order_relaxed);

    if (state == 0) {
      if (try_lock()) {
        return true;
      }

      continue;
    }

    // Nobody goes ahead of a handoff, and with the owner gone, it comes down to the line.
    const Thread* owner = owner_of(state);
    if (owner == nullptr || (state & HANDOFF) != 0) {
      return false;
    }

    const std::uint32_t cpu = m_owner_cpu.load(std::memory_order_relaxed);
    if (!hw::smp::is_online(cpu) || Scheduler::cpu_runqueue(cpu).current() != owner) {
      return false;
    }

    hw::cpu_relax();
  }

  return false;
}

// Tries to become the owner, which only works if nobody is, and if @p waiter has as much right to
// it as the first in line.
bool Mutex::try_take_locked(MutexWaiter& waiter) noexcept {
  std::uintptr_t state = m_state.load(std::memory_order_relaxed);

  while (owner_of(state) == nullptr) {
    const MutexWaiter* first = m_line.first();
    if (first != nullptr && first != &waiter && waiter.priority < first->priority) {
      return false;
    }

    const bool others_left =
        first != nullptr && (first != &waiter || Line::next(waiter) != nullptr);
    const std::uintptr_t next = word_of(waiter.thread) | (others_left ? WAITERS : 0);

    if (m_state.compare_exchange_weak(state, next, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
      return true;
    }
  }

  return false;
}

// Works out what @p thread inherits, and acts on it.
// @return Whether that changed anything.
// @pre Holds the thread's m_pi_lock.
bool Mutex::refresh_priority(Thread& thread) noexcept {
  Priority best = thread.m_base_priority;
  for (const Mutex& mutex : thread.m_contended_mutexes) {
    best = std::max(best, mutex.m_donation);
  }

  if (best == thread.m_effective_priority) {
    return false;
  }

  thread.m_effective_priority = best;
  Scheduler::apply_priority(thread, best);
  return true;
}

// The thread of @p waiter is the owner now. Takes it out of line, and starts lending it the
// priority of whoever is left.
void Mutex::settle_owner_locked(MutexWaiter& waiter) noexcept {
  if (waiter.in_tree()) {
    m_line.erase(waiter);
  }

  Thread& owner = *waiter.thread;
  utils::NakedGuard pi_guard{owner.m_pi_lock};

  owner.m_blocked_on = nullptr;

  if (const MutexWaiter* first = m_line.first(); first != nullptr) {
    m_donation = first->priority;
    owner.m_contended_mutexes.push_front(*this);
    (void)refresh_priority(owner);
  }
}

// Gets @p waiter in line if it is not yet, and makes sure that the owner can tell.
// @return false if the lock turned out to be free, so that there is no point in sleeping.
bool Mutex::stand_in_line_locked(MutexWaiter& waiter) noexcept {
  Thread& self = *waiter.thread;
  const bool first_visit = !waiter.in_tree();

  if (first_visit) {
    utils::NakedGuard pi_guard{self.m_pi_lock};

    waiter.priority = self.m_effective_priority;
    self.m_blocked_on = &waiter;
    m_line.insert(waiter);
  }

  // Back here after being woken, first in line, and the lock is gone: enough of that.
  const std::uintptr_t flags = WAITERS | (!first_visit && m_line.first() == &waiter ? HANDOFF : 0);

  std::uintptr_t state = m_state.load(std::memory_order_relaxed);
  while (true) {
    // It came free. Going around takes it, but while it is on its way, a line with nothing to show
    // for it would let someone lock this and unlock it again without ever looking at the line.
    if (state == 0) {
      if (m_state.compare_exchange_weak(state, WAITERS, std::memory_order_relaxed)) {
        return false;
      }

      continue;
    }

    // Free, but someone is in line. That one was woken, and it takes it from here.
    if (owner_of(state) == nullptr || (state & flags) == flags) {
      return true;
    }

    if (m_state.compare_exchange_weak(state, state | flags, std::memory_order_relaxed)) {
      return true;
    }
  }
}

/// One step down a chain: @p owner got a new priority, and if it is in line somewhere, its place
/// there has to change.
///
/// Taking m_wait_lock after m_pi_lock goes against the lock order, so this has to be able to back
/// off. The owner has to stay around for that, as without its m_pi_lock nothing holds it here.
///
/// @param deadlock_on Cleared if this says nothing about a deadlock: see propagate().
/// @param retain      Called with @p owner before this lets go of it.
/// @return The next mutex with its m_wait_lock held, or null at the end of the chain.
/// @pre Holds the m_pi_lock of @p owner, and does not when this returns.
template <typename Retain>
Mutex* Mutex::next_in_chain(Thread* owner, const Thread*& deadlock_on, Retain&& retain) noexcept {
  while (true) {
    MutexWaiter* place = owner->m_blocked_on;
    if (place == nullptr) {
      owner->m_pi_lock.unlock();
      return nullptr;
    }

    Mutex* next = place->mutex;
    if (next->m_wait_lock.try_lock()) {
      next->m_line.erase(*place);
      place->priority = owner->m_effective_priority;
      next->m_line.insert(*place);

      if (place->has_time_limit) {
        deadlock_on = nullptr;
      }

      owner->m_pi_lock.unlock();
      return next;
    }

    // Nothing keeps the owner from moving on while this lets go. What it is in line for next may
    // have nothing to do with the steps so far, so a circle would not mean a thing any more.
    retain(owner);
    deadlock_on = nullptr;

    owner->m_pi_lock.unlock();
    hw::cpu_relax();
    owner->m_pi_lock.lock();
  }
}

/// Brings the owner of @p mutex in line with who is waiting for it, then the owner of whatever that
/// one waits for, and so on until nothing changes any more.
///
/// Each step finishes before the next one starts, with no lock held across them that the next one
/// would need. If the chain changes in the meantime, whoever changed it saw what was done so far,
/// and takes it from there.
///
/// @param deadlock_on Reaching this thread as an owner means a deadlock, as every step of the way
///                    held for as long as it took to get to the next. Not if a wait on the way has
///                    a time limit, which gets everyone out of it by itself.
/// @pre Holds the m_wait_lock of @p mutex, and does not when this returns. Preemption is off.
void Mutex::propagate(Mutex* mutex, const Thread* deadlock_on) noexcept {
  Thread* retained = nullptr;
  WakeList wakes;

  // Holds on to one thread at a time, for when nothing else does.
  const auto retain = [&retained](Thread* thread) {
    if (retained != thread) {
      if (retained != nullptr) {
        retained->release();
      }

      thread->retain();
      retained = thread;
    }
  };

  while (mutex != nullptr) {
    Thread* owner = mutex->owner();
    const MutexWaiter* first = mutex->m_line.first();

    if (owner == nullptr) {
      // With the lock free the first in line is the one to take it, and that can be a thread that
      // just got ahead of the one that was woken for it.
      if (first != nullptr) {
        wakes.add(*first->thread);
      } else {
        mutex->clear_flags();
      }

      mutex->m_wait_lock.unlock();
      break;
    }

    if (owner == deadlock_on) [[unlikely]] {
      utils::logger::fatal("sched: thread {} would wait for itself through a Mutex\n",
                           owner->tid());
    }

    owner->m_pi_lock.lock();

    if (first != nullptr) {
      mutex->m_donation = first->priority;
      if (!mutex->is_linked()) {
        owner->m_contended_mutexes.push_front(*mutex);
      }
    } else if (mutex->is_linked()) {
      owner->m_contended_mutexes.remove(*mutex);
    }

    const bool changed = refresh_priority(*owner);

    // Last, and in this order. The flags are all that sends the owner through m_wait_lock, so from
    // here on it can unlock and exit without this code ever hearing about it. And a thread that
    // has unlocked everything should find itself back at its own priority, not on its way there.
    if (first == nullptr) {
      retain(owner);
      mutex->clear_flags();
    }

    mutex->m_wait_lock.unlock();

    if (!changed) {
      owner->m_pi_lock.unlock();
      break;
    }

    mutex = next_in_chain(owner, deadlock_on, retain);
  }

  if (retained != nullptr) {
    retained->release();
  }

  wakes.wake_all();
}

// Takes a thread that gave up out of line. @pre Holds m_wait_lock, and does not when this returns.
void Mutex::leave_line_locked(MutexWaiter& waiter) noexcept {
  if (!waiter.in_tree()) {
    m_wait_lock.unlock();
    return;
  }

  const bool was_first = m_line.first() == &waiter;
  m_line.erase(waiter);

  {
    utils::NakedGuard pi_guard{waiter.thread->m_pi_lock};
    waiter.thread->m_blocked_on = nullptr;
  }

  // Being first is the only way to leave an empty line, so nothing is forgotten by not looking.
  if (was_first) {
    propagate(this, nullptr);
  } else {
    m_wait_lock.unlock();
  }
}

void Mutex::clear_flags() noexcept {
  std::uintptr_t state = m_state.load(std::memory_order_relaxed);
  while (!m_state.compare_exchange_weak(state, state & ~FLAGS, std::memory_order_relaxed)) {
  }
}

bool Mutex::lock_slow(const klib::TimePoint* deadline) noexcept {
  Thread* self = current();

  KASSERT(self != nullptr && hw::percpu::can_sleep() && hw::irq::is_enabled());
  KASSERT(owner() != self);

  if (spin_on_owner()) {
    return true;
  }

  Alarm alarm;
  if (deadline != nullptr) {
    alarm.set(*deadline);
  }

  MutexWaiter waiter{self, this, deadline != nullptr};
  bool acquired = false;

  while (true) {
    hw::percpu::preempt_disable();
    m_wait_lock.lock();

    if (waiter.granted) {
      acquired = true;
      m_wait_lock.unlock();
      break;
    }

    if (!waiter.in_tree()) {
      utils::NakedGuard pi_guard{self->m_pi_lock};
      waiter.priority = self->m_effective_priority;
    }

    if (try_take_locked(waiter)) {
      note_owner_cpu();
      settle_owner_locked(waiter);
      acquired = true;
      m_wait_lock.unlock();
      break;
    }

    // After trying: the lock is owned at this point, or the line has someone who will get it, so
    // whoever comes next can be left to deal with it.
    if (alarm.rang()) {
      leave_line_locked(waiter);
      break;
    }

    if (!stand_in_line_locked(waiter)) {
      m_wait_lock.unlock();
      hw::percpu::preempt_enable();
      continue;
    }

    Scheduler::prepare_to_block();
    propagate(this, deadline == nullptr ? self : nullptr);

    hw::percpu::preempt_enable();
    Scheduler::schedule();
  }

  // An ordinary thread is running again if it got here. A boot thread never left, and the wake that
  // marks it as running can be a moment behind whatever let it through.
  Scheduler::cancel_block();

  hw::percpu::preempt_enable();
  return acquired;
}

void Mutex::unlock_slow() noexcept {
  Thread* self = current();
  WakeList wakes;

  hw::percpu::preempt_disable();
  m_wait_lock.lock();

  const std::uintptr_t state = m_state.load(std::memory_order_relaxed);
  KASSERT(owner_of(state) == self);

  if (is_linked()) {
    utils::NakedGuard pi_guard{self->m_pi_lock};
    self->m_contended_mutexes.remove(*this);
    (void)refresh_priority(*self);
  }

  // Plain stores are fine here. Everyone else needs m_wait_lock with these flags set, and without
  // them all a compare-and-swap could expect is 0, which this is not.
  MutexWaiter* first = m_line.first();
  if (first == nullptr) {
    m_state.store(0, std::memory_order_release);
  } else if ((state & HANDOFF) != 0) {
    const bool others_left = Line::next(*first) != nullptr;

    m_state.store(word_of(first->thread) | (others_left ? WAITERS : 0), std::memory_order_release);
    m_owner_cpu.store(first->thread->cpu_id(), std::memory_order_relaxed);

    first->granted = true;
    settle_owner_locked(*first);
    wakes.add(*first->thread);
  } else {
    m_state.store(WAITERS, std::memory_order_release);
    wakes.add(*first->thread);
  }

  m_wait_lock.unlock();

  wakes.wake_all();
  hw::percpu::preempt_enable();
}

bool Mutex::try_lock_for(const klib::Duration timeout) noexcept {
  if (try_lock()) {
    return true;
  }

  const klib::TimePoint deadline = drivers::SystemClock::now() + timeout;
  return lock_slow(&deadline);
}
}  // namespace kernel::sched
