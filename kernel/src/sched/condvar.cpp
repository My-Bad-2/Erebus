#include "sched/condvar.hpp"

#include "drivers/clock.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"

namespace kernel::sched {
bool CondVar::wait_locked(Mutex& mutex, const klib::TimePoint* deadline) noexcept {
  Alarm alarm;
  if (deadline != nullptr) {
    alarm.set(*deadline);
  }

  WaitQueue::Waiter waiter;
  m_waiters.prepare(waiter);

  // It is on the queue now, so a notification finds it, however this goes. Unlocking cannot block,
  // and if it gets preempted, the loop sees to it.
  mutex.unlock();

  while (true) {
    Scheduler::prepare_to_block();
    if (!waiter.is_queued() || alarm.rang()) {
      break;
    }

    Scheduler::schedule();
  }

  const bool notified = m_waiters.finish(waiter);
  mutex.lock();
  return notified;
}

void CondVar::wait(Mutex& mutex) noexcept { (void)wait_locked(mutex, nullptr); }

bool CondVar::wait_for(Mutex& mutex, const klib::Duration timeout) noexcept {
  return wait_until(mutex, drivers::SystemClock::now() + timeout);
}

bool CondVar::wait_until(Mutex& mutex, const klib::TimePoint deadline) noexcept {
  return wait_locked(mutex, &deadline);
}
}  // namespace kernel::sched
