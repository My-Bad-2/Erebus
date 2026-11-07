// Sleeping on real timers. These check for "at least" exactly and for "not much more" generously:
// a VM can lose a CPU for a while at any moment, and only the first of these is a promise.
#include "sched/sleep.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using klib::TimePoint;
using sched::Alarm;
using sched::Scheduler;
using sched::Thread;
using sched::ThreadState;
using support::A_MOMENT;
using support::start;
using support::wait_for;

constexpr Duration NAP = Duration::milliseconds(5);

struct Nap {
  Duration length{NAP};
  std::atomic<Thread*> self{nullptr};
  std::atomic<std::uint64_t> slept_ns{0};
  std::atomic<bool> finished_normally{false};
  std::atomic<bool> done{false};
};

void take_a_nap(Nap* nap) noexcept {
  Thread* self = sched::current();
  nap->self.store(self, std::memory_order_release);

  const TimePoint start_time = drivers::SystemClock::now();

  const bool finished = sched::sleep_for(nap->length);

  nap->slept_ns.store((drivers::SystemClock::now() - start_time).to_ns(),
                      std::memory_order_release);
  nap->finished_normally.store(finished, std::memory_order_release);
  nap->done.store(true, std::memory_order_release);
}

[[nodiscard]] bool is_sleeping(const Nap& nap) noexcept {
  const Thread* thread = nap.self.load(std::memory_order_acquire);
  return thread != nullptr && thread->state() == ThreadState::Sleeping;
}

// Goes back to bed until it has been told to stop, or runs out of time.
struct Dreamer {
  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> released{false};
  std::atomic<std::uint32_t> woken{0};
  std::atomic<bool> done{false};
};

void keep_sleeping(Dreamer* dreamer) noexcept {
  dreamer->self.store(sched::current(), std::memory_order_release);

  while (!dreamer->released.load(std::memory_order_acquire)) {
    if (sched::sleep_for(Duration::seconds(60))) {
      break;
    }

    dreamer->woken.fetch_add(1, std::memory_order_acq_rel);
  }

  dreamer->done.store(true, std::memory_order_release);
}

// Sets an alarm and leaves before it goes off, over and over, with a stack that gets reused.
struct AlarmPayload {
  std::atomic<std::uint32_t> rounds_left{0};
  std::atomic<bool> done{false};
};

void set_and_abandon_alarms(AlarmPayload* payload) noexcept {
  while (payload->rounds_left.fetch_sub(1, std::memory_order_acq_rel) != 0) {
    const Alarm alarm{drivers::SystemClock::now() + Duration::microseconds(20)};

    // Sometimes it goes off first, sometimes it is still running, sometimes it is still waiting.
    support::spin_for(Duration::microseconds(payload->rounds_left.load() % 40));
  }

  payload->done.store(true, std::memory_order_release);
}
}  // namespace

KTEST(Sleep, sleeps_for_at_least_as_long_as_it_was_told) {
  Nap nap;
  KREQUIRE(start("nap", &take_a_nap, &nap) != nullptr);
  KREQUIRE(wait_for([&] { return nap.done.load(std::memory_order_acquire); }));

  KEXPECT_GE(nap.slept_ns.load(), static_cast<std::uint64_t>(NAP.to_ns()));
  KEXPECT(nap.finished_normally.load());
}

// The whole point of it. A sleep that polled would pass the test above, and asking the thread how
// much CPU time it got would not tell: that only gets added up when a thread leaves its CPU.
KTEST(Sleep, a_sleeping_thread_gives_up_the_cpu) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("the test needs a CPU of its own to watch from");
  }

  const std::uint32_t cpu = support::last_cpu();
  const sched::RunQueue& rq = Scheduler::cpu_runqueue(cpu);

  Nap nap;
  nap.length = Duration::seconds(60);

  KREQUIRE(start("nap", &take_a_nap, &nap, support::pinned_to(cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return is_sleeping(nap); }));

  support::spin_for(A_MOMENT);
  KEXPECT(rq.current() == rq.idle_thread());
  KEXPECT(is_sleeping(nap));

  sched::cancel_sleep(nap.self.load());
  KEXPECT(wait_for([&] { return nap.done.load(std::memory_order_acquire); }));
}

KTEST(Sleep, a_deadline_in_the_past_does_not_sleep_at_all) {
  const std::uint64_t switches = hw::percpu::runqueue().switches();
  const TimePoint before = drivers::SystemClock::now();

  KEXPECT(sched::sleep_until(before - Duration::seconds(1)));
  KEXPECT(sched::sleep_for(Duration{}));

  KEXPECT_EQ(hw::percpu::runqueue().switches(), switches);
  KEXPECT(sched::current()->state() == ThreadState::Running);
}

KTEST(Sleep, the_boot_thread_can_sleep_too) {
  const TimePoint before = drivers::SystemClock::now();

  KEXPECT(sched::sleep_for(Duration::milliseconds(2)));

  KEXPECT_GE((drivers::SystemClock::now() - before).to_ns(), Duration::milliseconds(2).to_ns());
  KEXPECT(sched::current()->state() == ThreadState::Running);
}

KTEST(Sleep, sleepers_wake_up_in_the_order_of_their_deadlines) {
  static constexpr std::uint32_t SLEEPERS = 4;

  std::array<Nap, SLEEPERS> naps;
  for (std::uint32_t i = 0; i < SLEEPERS; ++i) {
    // Started the other way around, so that the first thread is the last to wake.
    naps[i].length = Duration::milliseconds(40) - Duration::milliseconds(8) * i;
    KREQUIRE(start("nap", &take_a_nap, &naps[i]) != nullptr);
  }

  KREQUIRE(wait_for([&] {
    for (const Nap& nap : naps) {
      if (!nap.done.load(std::memory_order_acquire)) {
        return false;
      }
    }

    return true;
  }));

  for (const Nap& nap : naps) {
    KEXPECT_GE(nap.slept_ns.load(), static_cast<std::uint64_t>(nap.length.to_ns()));
  }
}

KTEST(Sleep, cancel_sleep_ends_a_sleep_early) {
  Nap nap;
  nap.length = Duration::seconds(60);

  KREQUIRE(start("nap", &take_a_nap, &nap) != nullptr);
  KREQUIRE(wait_for([&] { return is_sleeping(nap); }));

  sched::cancel_sleep(nap.self.load());

  KREQUIRE(wait_for([&] { return nap.done.load(std::memory_order_acquire); }));
  KEXPECT(!nap.finished_normally.load());
  KEXPECT_LT(nap.slept_ns.load(), static_cast<std::uint64_t>(Duration::seconds(30).to_ns()));
}

// Otherwise a thread on its way to sleep would miss it, and sleep the whole 60 seconds.
KTEST(Sleep, cancel_sleep_also_counts_for_the_next_sleep) {
  Thread* self = sched::current();

  sched::cancel_sleep(self);

  const TimePoint before = drivers::SystemClock::now();
  KEXPECT(!sched::sleep_for(Duration::seconds(60)));
  KEXPECT_LT((drivers::SystemClock::now() - before).to_ns(), Duration::seconds(30).to_ns());

  // And only for that one.
  KEXPECT(sched::sleep_for(Duration::microseconds(100)));
}

KTEST(Sleep, a_sleeper_can_be_cancelled_over_and_over) {
  Dreamer dreamer;
  KREQUIRE(start("dreamer", &keep_sleeping, &dreamer) != nullptr);
  KREQUIRE(wait_for([&] { return dreamer.self.load() != nullptr; }));

  sched::cancel_sleep(dreamer.self.load());
  KEXPECT(wait_for([&] { return dreamer.woken.load() == 1; }));

  dreamer.released.store(true, std::memory_order_release);
  sched::cancel_sleep(dreamer.self.load());
  KEXPECT(wait_for([&] { return dreamer.done.load(std::memory_order_acquire); }));
}

KTEST(Alarm, an_alarm_that_was_not_set_does_not_ring) {
  const Alarm alarm;

  support::spin_for(Duration::microseconds(100));
  KEXPECT(!alarm.rang());
}

KTEST(Alarm, rings_right_away_for_a_moment_in_the_past) {
  const Alarm alarm{drivers::SystemClock::now() - Duration::milliseconds(1)};
  KEXPECT(alarm.rang());
}

KTEST(Alarm, rings_at_its_deadline) {
  const TimePoint deadline = drivers::SystemClock::now() + Duration::milliseconds(1);
  const Alarm alarm{deadline};

  KEXPECT(!alarm.rang());
  KEXPECT(wait_for([&] { return alarm.rang(); }));
  KEXPECT_GE(drivers::SystemClock::now(), deadline);
}

// The timer is on the stack that the next round of the loop uses again. The queue would notice.
KTEST(Alarm, can_be_left_behind_at_any_moment) {
  AlarmPayload payloads[2];
  for (AlarmPayload& payload : payloads) {
    payload.rounds_left.store(3000, std::memory_order_relaxed);
    KREQUIRE(start("alarms", &set_and_abandon_alarms, &payload) != nullptr);
  }

  KEXPECT(wait_for([&] {
    return payloads[0].done.load(std::memory_order_acquire) &&
           payloads[1].done.load(std::memory_order_acquire);
  }));

  KEXPECT(hw::percpu::timers().is_consistent());
}
}  // namespace kernel::testing
