#include "sched/sleep.hpp"

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"

namespace kernel::sched {
hw::timer::TimerAction Alarm::ring(hw::timer::Timer&, void* alarm) noexcept {
  auto* self = static_cast<Alarm*>(alarm);

  // The owner cannot leave before this returns, as its destructor waits for that, so this is safe
  // even if it sees the flag and stops waiting on the spot.
  self->m_rang.store(true, std::memory_order_release);
  (void)Scheduler::wake(self->m_thread);

  return hw::timer::TimerAction::Done;
}

void Alarm::set(const klib::TimePoint deadline, const klib::Duration slack) noexcept {
  m_thread = current();

  if (deadline <= drivers::SystemClock::now()) {
    m_rang.store(true, std::memory_order_release);
    return;
  }

  m_timer.set_callback(&ring, this);
  (void)m_timer.start_at(deadline, slack);
}

bool sleep_until(const klib::TimePoint deadline, const klib::Duration slack) noexcept {
  KASSERT(hw::percpu::can_sleep() && hw::irq::is_enabled());

  Thread* self = current();
  const Alarm alarm{deadline, slack};
  bool cancelled = false;

  // Sleeping first, then looking: whatever happens after the look finds the thread asleep, and
  // makes it come back here.
  while (true) {
    self->set_state(ThreadState::Sleeping);

    cancelled = self->take_sleep_cancel();
    if (cancelled || alarm.rang()) {
      break;
    }

    Scheduler::schedule();
  }

  Scheduler::cancel_block();
  return !cancelled;
}

bool sleep_for(const klib::Duration duration, const klib::Duration slack) noexcept {
  return sleep_until(drivers::SystemClock::now() + duration, slack);
}

void cancel_sleep(Thread* thread) noexcept {
  thread->request_sleep_cancel();

  // Only a sleeper. Anything else waiting would take this for spurious and go back to waiting,
  // which is fine, but the mark is all it takes if it gets to a sleep later.
  if (thread->state() == ThreadState::Sleeping) {
    (void)Scheduler::wake(thread);
  }
}
}  // namespace kernel::sched
