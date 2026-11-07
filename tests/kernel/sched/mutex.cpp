// The Mutex. First that it excludes, then who gets it next, and then the part that takes the most
// machinery: that what a waiter is worth reaches the thread standing in its way, wherever that is.
#include "sched/mutex.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using sched::Mutex;
using sched::Priority;
using sched::PriorityClass;
using sched::Thread;
using sched::ThreadState;
using support::A_MOMENT;
using support::of_class;
using support::start;
using support::wait_for;

static_assert(utils::BasicLockable<Mutex>);
static_assert(alignof(Thread) >= 64);

// A thread that takes a lock, holds on to it until told otherwise, and reports on all of that.
struct Holder {
  Mutex* mutex{nullptr};
  Mutex* second{nullptr};  // Taken after the first, while holding it.
  std::atomic<std::uint32_t>* tickets{nullptr};

  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> holding{false};
  std::atomic<bool> holding_second{false};
  std::atomic<std::uint32_t> ticket{~0u};
  std::atomic<bool> let_go_second{false};
  std::atomic<bool> let_go{false};
  std::atomic<PriorityClass> class_after{PriorityClass::Idle};
  std::atomic<std::uint32_t> weight_after{0};
  std::atomic<bool> done{false};
};

void hold(Holder* holder) noexcept {
  Thread* self = sched::current();
  holder->self.store(self, std::memory_order_release);

  holder->mutex->lock();
  if (holder->tickets != nullptr) {
    holder->ticket.store(holder->tickets->fetch_add(1, std::memory_order_acq_rel),
                         std::memory_order_release);
  }

  holder->holding.store(true, std::memory_order_release);

  if (holder->second != nullptr) {
    holder->second->lock();
    holder->holding_second.store(true, std::memory_order_release);
    support::sleep_until_set(holder->let_go_second);
    holder->second->unlock();
  }

  support::sleep_until_set(holder->let_go);
  holder->mutex->unlock();

  holder->class_after.store(self->priority_class(), std::memory_order_release);
  holder->weight_after.store(self->weight(), std::memory_order_release);
  holder->done.store(true, std::memory_order_release);
}

[[nodiscard]] Thread* thread_of(const Holder& holder) noexcept {
  return holder.self.load(std::memory_order_acquire);
}

[[nodiscard]] bool is_blocked(const Holder& holder) noexcept {
  const Thread* thread = thread_of(holder);
  return thread != nullptr && thread->state() == ThreadState::Blocked;
}

// Class and weight change together under a lock this does not hold, so it takes a look at both.
[[nodiscard]] bool has_class(const Holder& holder, const PriorityClass priority) noexcept {
  const Thread* thread = thread_of(holder);
  return thread != nullptr && thread->priority_class() == priority &&
         thread->weight() == sched::priority_to_weight(priority);
}

void let_go(Holder& holder) noexcept { holder.let_go.store(true, std::memory_order_release); }

// Takes two locks and lets go of them the other way around from hold().
struct Swapper {
  Mutex* first{nullptr};
  Mutex* second{nullptr};

  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> has_first{false};
  std::atomic<bool> go_on{false};
  std::atomic<bool> has_both{false};
  std::atomic<bool> drop_first{false};
  std::atomic<bool> drop_second{false};
  std::atomic<PriorityClass> class_after{PriorityClass::Idle};
  std::atomic<bool> done{false};
};

void take_both(Swapper* swapper) noexcept {
  Thread* self = sched::current();
  swapper->self.store(self, std::memory_order_release);

  swapper->first->lock();
  swapper->has_first.store(true, std::memory_order_release);
  support::sleep_until_set(swapper->go_on);

  swapper->second->lock();
  swapper->has_both.store(true, std::memory_order_release);

  support::sleep_until_set(swapper->drop_first);
  swapper->first->unlock();

  support::sleep_until_set(swapper->drop_second);
  swapper->second->unlock();

  swapper->class_after.store(self->priority_class(), std::memory_order_release);
  swapper->done.store(true, std::memory_order_release);
}

struct Timed {
  Mutex* mutex{nullptr};
  Duration timeout{};
  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> acquired{false};
  std::atomic<std::uint64_t> took_ns{0};
  std::atomic<bool> done{false};
};

void lock_with_a_limit(Timed* timed) noexcept {
  timed->self.store(sched::current(), std::memory_order_release);

  const klib::TimePoint before = drivers::SystemClock::now();
  const bool acquired = timed->mutex->try_lock_for(timed->timeout);

  timed->took_ns.store((drivers::SystemClock::now() - before).to_ns(), std::memory_order_release);
  timed->acquired.store(acquired, std::memory_order_release);

  if (acquired) {
    timed->mutex->unlock();
  }

  timed->done.store(true, std::memory_order_release);
}

// Holds one lock and goes for another, within limits.
struct Patient {
  Mutex* held{nullptr};
  Mutex* wanted{nullptr};
  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> done{false};
};

void hold_and_try(Patient* patient) noexcept {
  patient->self.store(sched::current(), std::memory_order_release);

  patient->held->lock();

  if (patient->wanted->try_lock_for(Duration::milliseconds(50))) {
    patient->wanted->unlock();
  }

  patient->held->unlock();
  patient->done.store(true, std::memory_order_release);
}

struct Crowd {
  Mutex mutex{};
  std::uint64_t counter{0};  // Plain on purpose: this is what the Mutex has to protect.
  std::uint32_t rounds{0};
  std::atomic<std::uint32_t> running{0};
};

void increment(Crowd* crowd) noexcept {
  for (std::uint32_t round = 0; round < crowd->rounds; ++round) {
    utils::NakedGuard guard{crowd->mutex};

    const std::uint64_t value = crowd->counter;
    if (round % 256 == 0) {
      sched::Scheduler::yield();  // A long critical section, now and then, so that they queue up.
    }

    crowd->counter = value + 1;
  }

  crowd->running.fetch_sub(1, std::memory_order_acq_rel);
}

// Keeps taking a lock in a tight loop, so that nobody gets it by being lucky.
struct Hog {
  Mutex* mutex{nullptr};
  Duration hold{};
  std::atomic<bool> stop{false};
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
};

void hog(Hog* hog) noexcept {
  while (!hog->stop.load(std::memory_order_acquire)) {
    hog->mutex->lock();
    hog->started.store(true, std::memory_order_release);
    support::spin_for(hog->hold);
    hog->mutex->unlock();
  }

  hog->done.store(true, std::memory_order_release);
}

// Counts how many times it took being given the CPU to get the lock.
struct Latecomer {
  Mutex* mutex{nullptr};
  std::atomic<std::uint64_t> wakeups{0};
  std::atomic<bool> done{false};
};

void count_the_wakeups(Latecomer* latecomer) noexcept {
  const Thread* self = sched::current();
  const std::uint64_t before = self->stats().switch_count;

  latecomer->mutex->lock();

  latecomer->wakeups.store(self->stats().switch_count - before, std::memory_order_release);
  latecomer->mutex->unlock();
  latecomer->done.store(true, std::memory_order_release);
}

// Holds on until told, and is awake for all of it. Then looks at what happened first.
struct Demoted {
  Mutex* mutex{nullptr};
  const std::atomic<bool>* other_ran{nullptr};
  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> holding{false};
  std::atomic<bool> let_go{false};
  std::atomic<bool> saw_it_run{false};
  std::atomic<bool> done{false};
};

void hold_and_look(Demoted* demoted) noexcept {
  demoted->self.store(sched::current(), std::memory_order_release);

  demoted->mutex->lock();
  demoted->holding.store(true, std::memory_order_release);

  while (!demoted->let_go.load(std::memory_order_acquire)) {
    hw::pause();
  }

  demoted->mutex->unlock();

  demoted->saw_it_run.store(demoted->other_ran->load(std::memory_order_acquire),
                            std::memory_order_release);
  demoted->done.store(true, std::memory_order_release);
}

// Spins for a fixed time, doing nothing for anyone: the thread in the middle of an inversion.
struct Bystander {
  Duration time{};
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
};

void stand_in_the_way(Bystander* bystander) noexcept {
  bystander->started.store(true, std::memory_order_release);
  support::spin_for(bystander->time);
  bystander->done.store(true, std::memory_order_release);
}

struct Crossing {
  Mutex* first{nullptr};
  Mutex* second{nullptr};
  std::atomic<std::uint32_t>* at_the_barrier{nullptr};
  std::atomic<bool> got_second{true};
  std::atomic<bool> done{false};
};

void cross_over(Crossing* crossing) noexcept {
  crossing->first->lock();

  crossing->at_the_barrier->fetch_add(1, std::memory_order_acq_rel);
  while (crossing->at_the_barrier->load(std::memory_order_acquire) < 2) {
    (void)sched::sleep_for(Duration::microseconds(50));
  }

  const bool got = crossing->second->try_lock_for(Duration::milliseconds(20));
  crossing->got_second.store(got, std::memory_order_release);

  if (got) {
    crossing->second->unlock();
  }

  crossing->first->unlock();
  crossing->done.store(true, std::memory_order_release);
}

// Random priorities over a random set of locks, always taken in the same order, some with a time
// limit. At the end of it all, the thread must be what it was at the start.
struct Turmoil {
  std::array<Mutex, 4> mutexes{};
  std::array<std::uint64_t, 4> counters{};
  std::atomic<std::uint32_t> running{0};
  std::atomic<std::uint32_t> priority_leaks{0};
  std::atomic<std::uint64_t> seed{1};
  std::uint32_t rounds{0};
};

void stir(Turmoil* turmoil) noexcept {
  Thread* self = sched::current();
  support::Rng rng{turmoil->seed.fetch_add(1, std::memory_order_relaxed)};

  for (std::uint32_t round = 0; round < turmoil->rounds; ++round) {
    std::array<bool, 4> held{};

    for (std::uint32_t i = 0; i < turmoil->mutexes.size(); ++i) {
      if (rng.below(2) == 0) {
        continue;
      }

      if (rng.below(4) == 0) {
        held[i] = turmoil->mutexes[i].try_lock_for(Duration::microseconds(1 + rng.below(200)));
      } else {
        turmoil->mutexes[i].lock();
        held[i] = true;
      }

      if (held[i]) {
        ++turmoil->counters[i];
      }
    }

    if (rng.below(8) == 0) {
      (void)sched::sleep_for(Duration::microseconds(rng.below(100)));
    }

    for (std::uint32_t i = turmoil->mutexes.size(); i-- > 0;) {
      if (held[i]) {
        ++turmoil->counters[i];
        turmoil->mutexes[i].unlock();
      }
    }

    if (self->base_priority().priority_class != self->priority_class() ||
        self->base_priority().weight != self->weight() || self->holds_contended_mutex()) {
      turmoil->priority_leaks.fetch_add(1, std::memory_order_relaxed);
    }
  }

  turmoil->running.fetch_sub(1, std::memory_order_acq_rel);
}

// A lock owner that keeps going to sleep, so that it gets moved around while it inherits.
struct Traveller {
  Mutex mutex{};
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> running{0};
};

void travel(Traveller* traveller) noexcept {
  while (!traveller->stop.load(std::memory_order_acquire)) {
    traveller->mutex.lock();
    for (std::uint32_t i = 0; i < 8; ++i) {
      (void)sched::sleep_for(Duration::microseconds(5));
      sched::Scheduler::yield();
    }

    traveller->mutex.unlock();
  }

  traveller->running.fetch_sub(1, std::memory_order_acq_rel);
}

void contest(Traveller* traveller) noexcept {
  while (!traveller->stop.load(std::memory_order_acquire)) {
    if (traveller->mutex.try_lock_for(Duration::microseconds(30))) {
      traveller->mutex.unlock();
    }
  }

  traveller->running.fetch_sub(1, std::memory_order_acq_rel);
}
}  // namespace

KTEST(Mutex, starts_out_free) {
  const Mutex mutex;
  KEXPECT(mutex.owner() == nullptr);
}

KTEST(Mutex, knows_who_holds_it) {
  Mutex mutex;

  mutex.lock();
  KEXPECT(mutex.owner() == sched::current());

  mutex.unlock();
  KEXPECT(mutex.owner() == nullptr);
}

KTEST(Mutex, try_lock_only_gets_a_free_lock) {
  Mutex mutex;
  Holder holder{.mutex = &mutex};

  KREQUIRE(mutex.try_lock());
  mutex.unlock();

  KREQUIRE(start("holder", &hold, &holder) != nullptr);
  KREQUIRE(wait_for([&] { return holder.holding.load(std::memory_order_acquire); }));

  KEXPECT(!mutex.try_lock());
  KEXPECT(mutex.owner() == thread_of(holder));

  let_go(holder);
  KEXPECT(wait_for([&] { return holder.done.load(std::memory_order_acquire); }));
  KEXPECT(mutex.try_lock());
  mutex.unlock();
}

// A boot thread is not taken off its CPU the way others are, so nothing else sets it back to
// running after a wait that nobody ended: this one gives up by itself, with nobody there to wake
// it.
KTEST(Mutex, the_boot_thread_comes_out_of_a_wait_running) {
  Mutex mutex;
  Holder holder{.mutex = &mutex};

  KREQUIRE(start("holder", &hold, &holder) != nullptr);
  KREQUIRE(wait_for([&] { return holder.holding.load(std::memory_order_acquire); }));

  KEXPECT(!mutex.try_lock_for(Duration::milliseconds(2)));
  KEXPECT(sched::current()->state() == ThreadState::Running);

  let_go(holder);
  KEXPECT(wait_for([&] { return holder.done.load(std::memory_order_acquire); }));
}

KTEST(Mutex, works_with_the_guards) {
  Mutex mutex;
  {
    utils::NakedGuard guard{mutex};
    KEXPECT(mutex.owner() == sched::current());
  }

  KEXPECT(mutex.owner() == nullptr);
}

KTEST(Mutex, keeps_threads_apart) {
  static constexpr std::uint32_t THREADS = 8;

  Crowd crowd;
  crowd.rounds = 4000;
  crowd.running.store(THREADS, std::memory_order_release);

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(start("crowd", &increment, &crowd) != nullptr);
  }

  KREQUIRE(wait_for([&] { return crowd.running.load(std::memory_order_acquire) == 0; }));
  KEXPECT_EQ(crowd.counter, std::uint64_t{THREADS} * crowd.rounds);
  KEXPECT(crowd.mutex.owner() == nullptr);
}

KTEST(Mutex, a_blocked_thread_goes_to_sleep) {
  Mutex mutex;
  Holder first{.mutex = &mutex};
  Holder second{.mutex = &mutex};

  KREQUIRE(start("first", &hold, &first) != nullptr);
  KREQUIRE(wait_for([&] { return first.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("second", &hold, &second) != nullptr);
  KEXPECT(wait_for([&] { return is_blocked(second); }));

  let_go(first);
  KEXPECT(wait_for([&] { return second.holding.load(std::memory_order_acquire); }));
  KEXPECT(mutex.owner() == thread_of(second));

  let_go(second);
  KEXPECT(wait_for([&] { return second.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return first.done.load(std::memory_order_acquire); }));
}

KTEST(Mutex, try_lock_for_gives_up_after_the_timeout) {
  Mutex mutex;
  Holder holder{.mutex = &mutex};
  Timed timed{.mutex = &mutex, .timeout = Duration::milliseconds(5)};

  KREQUIRE(start("holder", &hold, &holder) != nullptr);
  KREQUIRE(wait_for([&] { return holder.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("timed", &lock_with_a_limit, &timed) != nullptr);
  KREQUIRE(wait_for([&] { return timed.done.load(std::memory_order_acquire); }));

  KEXPECT(!timed.acquired.load());
  KEXPECT_GE(timed.took_ns.load(), static_cast<std::uint64_t>(timed.timeout.to_ns()));
  KEXPECT(mutex.owner() == thread_of(holder));

  // It left nothing behind: no flag to send the owner down the slow path, no one to wake.
  let_go(holder);
  KEXPECT(wait_for([&] { return holder.done.load(std::memory_order_acquire); }));
  KEXPECT(mutex.owner() == nullptr);
  KEXPECT(mutex.try_lock());
  mutex.unlock();
}

KTEST(Mutex, try_lock_for_gets_a_lock_that_comes_free) {
  Mutex mutex;
  Holder holder{.mutex = &mutex};
  Timed timed{.mutex = &mutex, .timeout = Duration::seconds(60)};

  KREQUIRE(start("holder", &hold, &holder) != nullptr);
  KREQUIRE(wait_for([&] { return holder.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("timed", &lock_with_a_limit, &timed) != nullptr);
  KREQUIRE(wait_for([&] {
    const Thread* thread = timed.self.load(std::memory_order_acquire);
    return thread != nullptr && thread->state() == ThreadState::Blocked;
  }));

  let_go(holder);
  KREQUIRE(wait_for([&] { return timed.done.load(std::memory_order_acquire); }));

  KEXPECT(timed.acquired.load());
  KEXPECT_LT(timed.took_ns.load(), static_cast<std::uint64_t>(Duration::seconds(30).to_ns()));
  KEXPECT(wait_for([&] { return holder.done.load(std::memory_order_acquire); }));
}

// Someone leaving the line is no reason for the ones behind it to get lost.
KTEST(Mutex, the_first_in_line_giving_up_leaves_the_rest_in_line) {
  Mutex mutex;
  Holder owner{.mutex = &mutex};
  Timed quitter{.mutex = &mutex, .timeout = Duration::milliseconds(10)};
  Holder patient{.mutex = &mutex};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("quitter", &lock_with_a_limit, &quitter, of_class(PriorityClass::High)) !=
           nullptr);
  KREQUIRE(wait_for([&] {
    const Thread* thread = quitter.self.load(std::memory_order_acquire);
    return thread != nullptr && thread->state() == ThreadState::Blocked;
  }));

  KREQUIRE(start("patient", &hold, &patient) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(patient); }));

  KEXPECT(wait_for([&] { return quitter.done.load(std::memory_order_acquire); }));
  KEXPECT(!quitter.acquired.load());

  let_go(owner);
  KEXPECT(wait_for([&] { return patient.holding.load(std::memory_order_acquire); }));

  let_go(patient);
  KEXPECT(wait_for([&] { return patient.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
}

KTEST(Mutex, the_most_important_waiter_gets_it_first) {
  Mutex mutex;
  std::atomic<std::uint32_t> tickets{0};
  Holder owner{.mutex = &mutex};
  std::array<Holder, 3> waiters;

  static constexpr std::array<PriorityClass, 3> CLASSES{PriorityClass::Normal, PriorityClass::High,
                                                        PriorityClass::RealTime};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));

  // Worst first, so that none of this comes down to who was there first.
  for (std::uint32_t i = 0; i < waiters.size(); ++i) {
    waiters[i].mutex = &mutex;
    waiters[i].tickets = &tickets;
    KREQUIRE(start("waiter", &hold, &waiters[i], of_class(CLASSES[i])) != nullptr);
    KREQUIRE(wait_for([&] { return is_blocked(waiters[i]); }));
  }

  let_go(owner);

  for (std::uint32_t i = waiters.size(); i-- > 0;) {
    KREQUIRE(wait_for([&] { return waiters[i].holding.load(std::memory_order_acquire); }));
    KEXPECT_EQ(waiters[i].ticket.load(), waiters.size() - 1 - i);
    let_go(waiters[i]);
  }

  for (const Holder& waiter : waiters) {
    KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  }

  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
}

KTEST(Mutex, nobody_gets_around_the_line_with_try_lock) {
  Mutex mutex;
  Holder owner{.mutex = &mutex};
  Holder waiter{.mutex = &mutex};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));
  KREQUIRE(start("waiter", &hold, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(waiter); }));

  KEXPECT(!mutex.try_lock());

  let_go(owner);
  KEXPECT(wait_for([&] { return waiter.holding.load(std::memory_order_acquire); }));
  KEXPECT(!mutex.try_lock());

  let_go(waiter);
  KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
}

// A thread running full tilt beats one that has to be woken, every time. The first in line can lose
// that race once and then gets the lock handed to it: one lost race and one win, so two wakeups.
// The other thread holds on for longer than spinning lasts, for this one to really go to sleep.
KTEST(Mutex, a_woken_thread_loses_the_race_only_once) {
  if (hw::smp::online_cpus() < 3) {
    KTEST_SKIP("needs a CPU each for two threads, and one for the test");
  }

  Mutex mutex;
  Hog hog_payload{.mutex = &mutex, .hold = Duration::microseconds(300)};
  KREQUIRE(start("hog", &hog, &hog_payload, support::pinned_to(support::last_cpu())) != nullptr);
  KREQUIRE(wait_for([&] { return hog_payload.started.load(std::memory_order_acquire); }));

  Latecomer latecomer{.mutex = &mutex};
  KREQUIRE(start("latecomer", &count_the_wakeups, &latecomer,
                 support::pinned_to(support::last_cpu() - 1)) != nullptr);

  const bool finished = wait_for([&] { return latecomer.done.load(std::memory_order_acquire); });

  hog_payload.stop.store(true, std::memory_order_release);
  KEXPECT(finished);
  KEXPECT(wait_for([&] { return latecomer.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return hog_payload.done.load(std::memory_order_acquire); }));

  // One more than it takes, for a stray wakeup from whatever else may be going on.
  KEXPECT_LE(latecomer.wakeups.load(), 3ull);
}

// With the lock free and the first in line not there yet, a thread out of time can still take it if
// it has the right to. Not having tried would be a strange reason to say it could not be done.
KTEST(Mutex, running_out_of_time_still_takes_a_lock_that_is_there) {
  if (hw::smp::online_cpus() < 3) {
    KTEST_SKIP("needs a CPU for each of the threads and the test");
  }

  const std::uint32_t busy_cpu = support::last_cpu();
  const std::uint32_t other_cpu = support::last_cpu() - 1;

  Mutex mutex;
  Holder owner{.mutex = &mutex};
  Holder first{.mutex = &mutex};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("first", &hold, &first, of_class(PriorityClass::High, busy_cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(first); }));

  Bystander bystander{.time = Duration::milliseconds(50)};
  KREQUIRE(start("in-the-way", &stand_in_the_way, &bystander,
                 of_class(PriorityClass::TestRunner, busy_cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return bystander.started.load(std::memory_order_acquire); }));

  let_go(owner);
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
  KEXPECT(mutex.owner() == nullptr);

  Timed impatient{.mutex = &mutex, .timeout = Duration{}};
  KREQUIRE(start("impatient", &lock_with_a_limit, &impatient,
                 of_class(PriorityClass::RealTime, other_cpu)) != nullptr);

  KEXPECT(wait_for([&] { return impatient.done.load(std::memory_order_acquire); }));
  KEXPECT(impatient.acquired.load());

  KEXPECT(wait_for([&] { return first.holding.load(std::memory_order_acquire); }));
  let_go(first);
  KEXPECT(wait_for([&] { return first.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return bystander.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, the_owner_gets_the_class_of_a_more_important_waiter) {
  Mutex mutex;
  Holder low{.mutex = &mutex};
  Holder high{.mutex = &mutex};

  KREQUIRE(start("low", &hold, &low) != nullptr);
  KREQUIRE(wait_for([&] { return low.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::RealTime)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(low, PriorityClass::RealTime); }));

  // What it is by itself did not change.
  KEXPECT(thread_of(low)->base_priority() == Priority::of(PriorityClass::Normal));

  let_go(low);
  KEXPECT(wait_for([&] { return low.done.load(std::memory_order_acquire); }));
  KEXPECT(low.class_after.load() == PriorityClass::Normal);
  KEXPECT_EQ(low.weight_after.load(), sched::priority_to_weight(PriorityClass::Normal));

  KEXPECT(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));
  let_go(high);
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, the_owner_gets_the_weight_of_a_heavier_waiter) {
  Mutex mutex;
  Holder low{.mutex = &mutex};
  Holder high{.mutex = &mutex};

  KREQUIRE(start("low", &hold, &low, of_class(PriorityClass::Background)) != nullptr);
  KREQUIRE(wait_for([&] { return low.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::High)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(low, PriorityClass::High); }));
  KEXPECT_EQ(thread_of(low)->weight(), sched::priority_to_weight(PriorityClass::High));

  let_go(low);
  KEXPECT(wait_for([&] { return low.done.load(std::memory_order_acquire); }));
  KEXPECT(low.class_after.load() == PriorityClass::Background);
  KEXPECT_EQ(low.weight_after.load(), sched::priority_to_weight(PriorityClass::Background));

  KEXPECT(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));
  let_go(high);
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, a_less_important_waiter_changes_nothing) {
  Mutex mutex;
  Holder high{.mutex = &mutex};
  Holder low{.mutex = &mutex};

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::High)) != nullptr);
  KREQUIRE(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("low", &hold, &low, of_class(PriorityClass::Background)) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(low); }));

  KEXPECT(has_class(high, PriorityClass::High));
  KEXPECT_EQ(thread_of(high)->weight(), sched::priority_to_weight(PriorityClass::High));

  let_go(high);
  KEXPECT(wait_for([&] { return low.holding.load(std::memory_order_acquire); }));
  let_go(low);
  KEXPECT(wait_for([&] { return low.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, goes_all_the_way_down_a_chain) {
  Mutex first;
  Mutex second;

  // `top` waits for `second`, held by `middle`, which waits for `first`, held by `bottom`.
  Holder bottom{.mutex = &first};
  Holder middle{.mutex = &second, .second = &first};
  Holder top{.mutex = &second};

  KREQUIRE(start("bottom", &hold, &bottom) != nullptr);
  KREQUIRE(wait_for([&] { return bottom.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("middle", &hold, &middle) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(middle); }));

  KREQUIRE(start("top", &hold, &top, of_class(PriorityClass::RealTime)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(middle, PriorityClass::RealTime); }));
  KEXPECT(wait_for([&] { return has_class(bottom, PriorityClass::RealTime); }));

  // Everyone falls back as soon as they stop being in the way.
  let_go(bottom);
  KEXPECT(wait_for([&] { return bottom.done.load(std::memory_order_acquire); }));
  KEXPECT(bottom.class_after.load() == PriorityClass::Normal);

  KEXPECT(wait_for([&] { return middle.holding_second.load(std::memory_order_acquire); }));
  middle.let_go_second.store(true, std::memory_order_release);
  let_go(middle);

  KEXPECT(wait_for([&] { return middle.done.load(std::memory_order_acquire); }));
  KEXPECT(middle.class_after.load() == PriorityClass::Normal);

  KEXPECT(wait_for([&] { return top.holding.load(std::memory_order_acquire); }));
  let_go(top);
  KEXPECT(wait_for([&] { return top.done.load(std::memory_order_acquire); }));
}

// What a thread is worth decides its place in line, and that can change while it stands there.
KTEST(MutexInheritance, inheriting_moves_a_thread_up_the_line) {
  Mutex contested;
  Mutex other;

  Holder owner{.mutex = &contested};

  // Normal, and in line behind `high` until someone worth more needs what it holds.
  Holder risen{.mutex = &other, .second = &contested};
  Holder high{.mutex = &contested};
  Holder real_time{.mutex = &other};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("risen", &hold, &risen) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(risen); }));

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::High)) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(high); }));

  // `high` is ahead now. This changes that: `risen` becomes real-time, and so does the owner.
  KREQUIRE(start("real-time", &hold, &real_time, of_class(PriorityClass::RealTime)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::RealTime); }));

  let_go(owner);
  KEXPECT(wait_for([&] { return risen.holding_second.load(std::memory_order_acquire); }));
  KEXPECT(!high.holding.load(std::memory_order_acquire));

  risen.let_go_second.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));

  let_go(risen);
  KEXPECT(wait_for([&] { return real_time.holding.load(std::memory_order_acquire); }));
  let_go(real_time);
  let_go(high);

  KEXPECT(wait_for([&] { return real_time.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return risen.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
}

// Letting go of the lock it inherited from must not take away what the other lock is worth.
KTEST(MutexInheritance, the_best_of_all_held_locks_counts) {
  Mutex important;
  Mutex ordinary;

  Holder owner{.mutex = &ordinary, .second = &important};
  Holder high{.mutex = &ordinary};
  Holder real_time{.mutex = &important};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding_second.load(std::memory_order_acquire); }));

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::High)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::High); }));

  KREQUIRE(start("real-time", &hold, &real_time, of_class(PriorityClass::RealTime)) != nullptr);
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::RealTime); }));

  owner.let_go_second.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::High); }));
  KEXPECT_EQ(thread_of(owner)->weight(), sched::priority_to_weight(PriorityClass::High));

  KEXPECT(wait_for([&] { return real_time.holding.load(std::memory_order_acquire); }));
  let_go(real_time);
  KEXPECT(wait_for([&] { return real_time.done.load(std::memory_order_acquire); }));

  let_go(owner);
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
  KEXPECT(owner.class_after.load() == PriorityClass::Normal);

  KEXPECT(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));
  let_go(high);
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
}

// The next owner is the most important of those in line, so the rest usually mean nothing to it.
// But it can be important only for the time being, because of another lock, and then they do.
KTEST(MutexInheritance, a_new_owner_falls_back_to_those_still_in_line) {
  Mutex wanted;
  Mutex other;

  Holder first_owner{.mutex = &wanted};
  Swapper swapper{.first = &other, .second = &wanted};
  Holder high{.mutex = &wanted};
  Holder real_time{.mutex = &other};

  KREQUIRE(start("first", &hold, &first_owner) != nullptr);
  KREQUIRE(wait_for([&] { return first_owner.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("high", &hold, &high, of_class(PriorityClass::High)) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(high); }));

  KREQUIRE(start("swapper", &take_both, &swapper) != nullptr);
  KREQUIRE(wait_for([&] { return swapper.has_first.load(std::memory_order_acquire); }));

  KREQUIRE(start("real-time", &hold, &real_time, of_class(PriorityClass::RealTime)) != nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(real_time); }));

  // Worth more than `high` now, so it gets `wanted` next, with `high` left in line behind it.
  swapper.go_on.store(true, std::memory_order_release);
  KREQUIRE(wait_for([&] {
    const Thread* thread = swapper.self.load(std::memory_order_acquire);
    return thread != nullptr && thread->state() == ThreadState::Blocked;
  }));

  let_go(first_owner);
  KREQUIRE(wait_for([&] { return swapper.has_both.load(std::memory_order_acquire); }));

  swapper.drop_first.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return real_time.holding.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return swapper.self.load()->priority_class() == PriorityClass::High; }));

  swapper.drop_second.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return swapper.done.load(std::memory_order_acquire); }));
  KEXPECT(swapper.class_after.load() == PriorityClass::Normal);

  let_go(real_time);
  KEXPECT(wait_for([&] { return real_time.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return high.holding.load(std::memory_order_acquire); }));
  let_go(high);
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return first_owner.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, giving_up_takes_back_what_was_lent) {
  Mutex mutex;
  Holder owner{.mutex = &mutex};
  Timed quitter{.mutex = &mutex, .timeout = Duration::milliseconds(20)};

  KREQUIRE(start("owner", &hold, &owner) != nullptr);
  KREQUIRE(wait_for([&] { return owner.holding.load(std::memory_order_acquire); }));

  KREQUIRE(start("quitter", &lock_with_a_limit, &quitter, of_class(PriorityClass::RealTime)) !=
           nullptr);
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::RealTime); }));

  KEXPECT(wait_for([&] { return quitter.done.load(std::memory_order_acquire); }));
  KEXPECT(!quitter.acquired.load());
  KEXPECT(wait_for([&] { return has_class(owner, PriorityClass::Normal); }));
  KEXPECT(!thread_of(owner)->holds_contended_mutex());

  let_go(owner);
  KEXPECT(wait_for([&] { return owner.done.load(std::memory_order_acquire); }));
}

// The textbook inversion, all on one CPU: `high` needs what `low` holds, and `middle` would keep
// `low` from ever finishing if nothing was done about it.
KTEST(MutexInheritance, a_thread_in_between_cannot_hold_up_a_more_important_one) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("the test itself needs a CPU to run on");
  }

  const std::uint32_t cpu = support::last_cpu();
  Mutex mutex;

  // Not one of the hold() threads: this one needs to run to get anywhere.
  Holder low{.mutex = &mutex};
  KREQUIRE(start("low", &hold, &low, of_class(PriorityClass::Background, cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return low.holding.load(std::memory_order_acquire); }));

  Bystander middle{.time = Duration::milliseconds(200)};
  KREQUIRE(start("middle", &stand_in_the_way, &middle, of_class(PriorityClass::RealTime, cpu)) !=
           nullptr);
  KREQUIRE(wait_for([&] { return middle.started.load(std::memory_order_acquire); }));

  // With `middle` spinning on this CPU, `low` only gets it by going ahead of it, which it can only
  // do by inheriting more than `middle` has.
  let_go(low);

  Timed high{.mutex = &mutex, .timeout = Duration::seconds(60)};
  const sched::ThreadCreateOptions runner = of_class(PriorityClass::TestRunner, cpu);
  KREQUIRE(start("high", &lock_with_a_limit, &high, runner) != nullptr);

  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
  KEXPECT(high.acquired.load());
  KEXPECT(!middle.done.load(std::memory_order_acquire));

  KEXPECT(wait_for([&] { return middle.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return low.done.load(std::memory_order_acquire); }));
}

// A wait with a time limit can get out of this by itself, so it must not be treated as fatal.
KTEST(MutexInheritance, a_deadlock_with_time_limits_breaks_itself) {
  Mutex one;
  Mutex other;
  std::atomic<std::uint32_t> at_the_barrier{0};

  Crossing first{.first = &one, .second = &other, .at_the_barrier = &at_the_barrier};
  Crossing second{.first = &other, .second = &one, .at_the_barrier = &at_the_barrier};

  KREQUIRE(start("cross", &cross_over, &first) != nullptr);
  KREQUIRE(start("cross", &cross_over, &second) != nullptr);

  KEXPECT(wait_for([&] {
    return first.done.load(std::memory_order_acquire) &&
           second.done.load(std::memory_order_acquire);
  }));

  // At least one of them timed out, or they would not have got out of it.
  KEXPECT(!first.got_second.load() || !second.got_second.load());
  KEXPECT(one.owner() == nullptr);
  KEXPECT(other.owner() == nullptr);
}

// One side waits forever and the other has a limit, so this ends by itself, and the walk down the
// chain must not take it for a deadlock. The thread closing the circle is the more important one,
// as the walk only goes on for as long as priorities change, and has to get all the way around.
KTEST(MutexInheritance, a_time_limit_in_the_chain_means_it_is_no_deadlock) {
  Mutex one;
  Mutex other;

  Swapper closer{.first = &other, .second = &one};
  closer.drop_first.store(true, std::memory_order_relaxed);
  closer.drop_second.store(true, std::memory_order_relaxed);

  KREQUIRE(start("closer", &take_both, &closer, of_class(PriorityClass::High)) != nullptr);
  KREQUIRE(wait_for([&] { return closer.has_first.load(std::memory_order_acquire); }));

  Patient timed{.held = &one, .wanted = &other};
  KREQUIRE(start("timed", &hold_and_try, &timed) != nullptr);
  KREQUIRE(wait_for([&] {
    const Thread* thread = timed.self.load(std::memory_order_acquire);
    return thread != nullptr && thread->state() == ThreadState::Blocked;
  }));

  closer.go_on.store(true, std::memory_order_release);

  KEXPECT(wait_for([&] { return timed.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return closer.done.load(std::memory_order_acquire); }));
}

// The thread in the way is real-time, so no slice timer is coming to the rescue, and the only thing
// that gets the owner going is being told from over here.
KTEST(MutexInheritance, reaches_an_owner_stuck_on_another_cpu) {
  if (hw::smp::online_cpus() < 3) {
    KTEST_SKIP("needs a CPU each for the two sides, and one for the test");
  }

  const std::uint32_t cpu = support::last_cpu();

  Mutex mutex;
  Holder low{.mutex = &mutex};
  KREQUIRE(start("low", &hold, &low, of_class(PriorityClass::Normal, cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return low.holding.load(std::memory_order_acquire); }));

  Bystander middle{.time = Duration::milliseconds(200)};
  KREQUIRE(start("middle", &stand_in_the_way, &middle, of_class(PriorityClass::RealTime, cpu)) !=
           nullptr);
  KREQUIRE(wait_for([&] { return middle.started.load(std::memory_order_acquire); }));

  // It will not see this until it gets the CPU back, and its last nap ends behind `middle`.
  let_go(low);
  support::spin_for(Duration::milliseconds(2));

  Timed high{.mutex = &mutex, .timeout = Duration::seconds(60)};
  KREQUIRE(start("high", &lock_with_a_limit, &high, of_class(PriorityClass::TestRunner, cpu - 1)) !=
           nullptr);

  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
  KEXPECT(high.acquired.load());
  KEXPECT(!middle.done.load(std::memory_order_acquire));

  KEXPECT(wait_for([&] { return middle.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return low.done.load(std::memory_order_acquire); }));
}

// It only stayed ahead of the thread behind it because of what it inherited. The same instant that
// is over, so is its turn, and a tick later would be too late.
KTEST(MutexInheritance, losing_what_it_inherited_gives_up_the_cpu_at_once) {
  if (hw::smp::online_cpus() < 3) {
    KTEST_SKIP("needs a CPU each for the two sides, and one for the test");
  }

  const std::uint32_t cpu = support::last_cpu();

  Mutex mutex;
  Bystander next{.time = Duration::milliseconds(1)};
  Demoted demoted{.mutex = &mutex, .other_ran = &next.started};

  KREQUIRE(start("demoted", &hold_and_look, &demoted, support::pinned_to(cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return demoted.holding.load(std::memory_order_acquire); }));

  Timed high{.mutex = &mutex, .timeout = Duration::seconds(60)};
  KREQUIRE(start("high", &lock_with_a_limit, &high, of_class(PriorityClass::TestRunner, cpu - 1)) !=
           nullptr);
  KREQUIRE(wait_for([&] {
    const Thread* thread = demoted.self.load(std::memory_order_acquire);
    return thread->priority_class() == PriorityClass::TestRunner;
  }));

  KREQUIRE(start("next", &stand_in_the_way, &next, of_class(PriorityClass::RealTime, cpu)) !=
           nullptr);

  support::spin_for(A_MOMENT);
  KEXPECT(!next.started.load(std::memory_order_acquire));

  demoted.let_go.store(true, std::memory_order_release);

  KEXPECT(wait_for([&] { return demoted.done.load(std::memory_order_acquire); }));
  KEXPECT(demoted.saw_it_run.load(std::memory_order_acquire));

  KEXPECT(wait_for([&] { return next.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return high.done.load(std::memory_order_acquire); }));
}

KTEST(MutexInheritance, reaches_a_thread_on_its_way_between_two_cpus) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("nothing to move between");
  }

  Traveller traveller;
  const std::uint32_t contenders = 3;
  traveller.running.store(2 + contenders, std::memory_order_release);

  KREQUIRE(start("travel", &travel, &traveller) != nullptr);
  KREQUIRE(start("travel", &travel, &traveller) != nullptr);

  for (std::uint32_t i = 0; i < contenders; ++i) {
    const PriorityClass priority = i == 0 ? PriorityClass::RealTime : PriorityClass::High;
    KREQUIRE(start("contest", &contest, &traveller, of_class(priority)) != nullptr);
  }

  support::spin_for(Duration::milliseconds(100));
  traveller.stop.store(true, std::memory_order_release);

  KEXPECT(wait_for([&] { return traveller.running.load(std::memory_order_acquire) == 0; }));
  KEXPECT(traveller.mutex.owner() == nullptr);
}

KTEST(MutexInheritance, everything_is_back_to_normal_after_a_lot_of_turmoil) {
  static constexpr std::array<PriorityClass, 4> CLASSES{PriorityClass::Background,
                                                        PriorityClass::Normal, PriorityClass::High,
                                                        PriorityClass::RealTime};
  static constexpr std::uint32_t THREADS = 12;

  Turmoil turmoil;
  turmoil.rounds = 4000;
  turmoil.running.store(THREADS, std::memory_order_release);

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(start("stir", &stir, &turmoil, of_class(CLASSES[i % CLASSES.size()])) != nullptr);
  }

  KREQUIRE(wait_for([&] { return turmoil.running.load(std::memory_order_acquire) == 0; }));
  KEXPECT_EQ(turmoil.priority_leaks.load(), 0u);

  for (const Mutex& mutex : turmoil.mutexes) {
    KEXPECT(mutex.owner() == nullptr);
  }

  // Every lock taken shows up exactly twice, once on the way in and once on the way out. Being
  // plain counters, they only add up if each was protected the whole time.
  for (const std::uint64_t counter : turmoil.counters) {
    KEXPECT_EQ(counter % 2, 0ull);
  }
}
}  // namespace kernel::testing
