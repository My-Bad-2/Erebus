// The real thing: real threads on real CPUs, with real timers. Everything here runs on top of
// what the tests next door have already taken apart, so these are about how it fits together, and
// about everything that only goes wrong with two CPUs at once.
#include "sched/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono.hpp>
#include <cstdint>
#include <span>

#include "../hal/fpu_registers.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "sched/fpu_guard.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
namespace smp = hw::smp;
using klib::Duration;
using sched::NumaDomain;
using sched::PriorityClass;
using sched::RunQueue;
using sched::Scheduler;
using sched::Thread;
using sched::ThreadCreateOptions;
using sched::ThreadState;

constexpr Duration PATIENCE = Duration::seconds(30);

// Gives the CPU to whatever wants it until @p done says so. A test that never gets there fails,
// rather than taking the whole run down with it.
template <typename Pred>
[[nodiscard]] bool wait_for(Pred&& done, const Duration timeout = PATIENCE) noexcept {
  const klib::TimePoint deadline = drivers::SystemClock::now() + timeout;

  while (!done()) {
    if (drivers::SystemClock::now() > deadline) {
      return false;
    }

    Scheduler::yield();
    hw::pause();
  }

  return true;
}

// The last CPU: an AP if there is one, as the BSP is busy running the test.
[[nodiscard]] std::uint32_t last_cpu() noexcept { return smp::online_cpus() - 1; }

[[nodiscard]] ThreadCreateOptions pinned_to(const std::uint32_t cpu) noexcept {
  return {.preferred_cpu = cpu, .pinned = true};
}

void spin_for(const Duration time) noexcept {
  const klib::TimePoint end = drivers::SystemClock::now() + time;
  while (drivers::SystemClock::now() < end) {
    hw::pause();
  }
}

struct Counter {
  std::atomic<std::uint32_t> done{0};
};

void count_once(Counter* counter) noexcept {
  counter->done.fetch_add(1, std::memory_order_acq_rel);
}

// A thread that does nothing but wait to be released, for when the test needs one to look at.
struct Parked {
  std::atomic<bool> running{false};
  std::atomic<bool> released{false};
  std::atomic<bool> finished{false};
  std::atomic<std::uint32_t> cpu{~0u};
};

void park(Parked* parked) noexcept {
  parked->cpu.store(hw::percpu::id(), std::memory_order_release);
  parked->running.store(true, std::memory_order_release);

  while (!parked->released.load(std::memory_order_acquire)) {
    hw::pause();
  }

  parked->finished.store(true, std::memory_order_release);
}

// Blocks until told otherwise, the way the file comment of scheduler.hpp says to.
struct Sleeper {
  std::atomic<Thread*> self{nullptr};
  std::atomic<std::uint32_t> wakeups_wanted{0};
  std::atomic<std::uint32_t> wakeups_seen{0};
  std::atomic<bool> asleep{false};
};

void wait_for_wakeup(Sleeper* sleeper) noexcept {
  while (true) {
    Scheduler::prepare_to_block();
    if (sleeper->wakeups_wanted.load(std::memory_order_acquire) >
        sleeper->wakeups_seen.load(std::memory_order_relaxed)) {
      Scheduler::cancel_block();
      break;
    }

    Scheduler::schedule();
  }

  sleeper->wakeups_seen.fetch_add(1, std::memory_order_release);
}

void sleep_until_woken(Sleeper* sleeper) noexcept {
  sleeper->self.store(sched::current(), std::memory_order_release);
  sleeper->asleep.store(true, std::memory_order_release);
  wait_for_wakeup(sleeper);
}

// Two threads handing a token back and forth, each blocking until it has it.
struct Baton {
  std::atomic<Thread*> players[2]{};
  std::atomic<std::uint32_t> turn{0};
  std::atomic<std::uint32_t> finished{0};
  std::uint32_t rounds{0};
};

struct Player {
  Baton* baton;
  std::uint32_t index;
};

void play(Player* player) noexcept {
  Baton& baton = *player->baton;
  const std::uint32_t me = player->index;

  baton.players[me].store(sched::current(), std::memory_order_release);
  while (baton.players[1 - me].load(std::memory_order_acquire) == nullptr) {
    Scheduler::yield();
  }

  for (std::uint32_t round = 0; round < baton.rounds; ++round) {
    while (true) {
      Scheduler::prepare_to_block();
      if (baton.turn.load(std::memory_order_acquire) % 2 == me) {
        Scheduler::cancel_block();
        break;
      }

      Scheduler::schedule();
    }

    baton.turn.fetch_add(1, std::memory_order_acq_rel);
    (void)Scheduler::wake(baton.players[1 - me].load(std::memory_order_acquire));
  }

  baton.finished.fetch_add(1, std::memory_order_acq_rel);
}

// Burns CPU without ever giving it up, and counts how much it got. Stops by itself, for the same
// reason FpuSpinner below does.
struct Spinner {
  klib::TimePoint until{};
  std::atomic<std::uint64_t> iterations{0};
  std::atomic<std::uint32_t>* finished{nullptr};
};

void spin(Spinner* spinner) noexcept {
  while (drivers::SystemClock::now() < spinner->until) {
    spinner->iterations.fetch_add(1, std::memory_order_relaxed);
  }

  spinner->finished->fetch_add(1, std::memory_order_acq_rel);
}

// Two of these on one CPU, each spinning until both have run. Without something taking the CPU
// away, the first one would get to the end of its patience with the other one still waiting.
struct TurnTaker {
  std::array<std::atomic<std::uint64_t>, 2>* progress{nullptr};
  std::uint32_t index{0};
  klib::TimePoint give_up_at{};
  std::atomic<std::uint32_t>* gave_up{nullptr};
  std::atomic<std::uint32_t>* finished{nullptr};
  std::uint64_t preempted{0};
};

void spin_until_both_ran(TurnTaker* me) noexcept {
  std::array<std::atomic<std::uint64_t>, 2>& progress = *me->progress;

  while (progress[0].load(std::memory_order_relaxed) < 1000 ||
         progress[1].load(std::memory_order_relaxed) < 1000) {
    progress[me->index].fetch_add(1, std::memory_order_relaxed);

    if (drivers::SystemClock::now() > me->give_up_at) {
      me->gave_up->fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }

  me->preempted = sched::current()->stats().involuntary_switches;
  me->finished->fetch_add(1, std::memory_order_acq_rel);
}

// What every thread should find when it starts through the scheduler.
struct FirstLook {
  std::atomic<bool> done{false};
  std::uint8_t preempt_count{0xFF};
  bool interrupts_on{false};
  bool lock_is_free{false};
};

// Registers that have to survive being taken off the CPU by an interrupt, at any moment.
//
// It finishes by itself after a set number of rounds. A test runs on the idle thread of its CPU,
// which gets no time at all while something else can run there, so a thread that had to be told
// to stop could end up keeping the only one who can tell it from ever running.
struct FpuSpinner {
  std::uint64_t seed{0};
  std::uint32_t rounds{0};
  std::atomic<std::uint32_t>* finished{nullptr};
  std::atomic<std::uint32_t>* mismatches{nullptr};
  std::atomic<std::uint32_t>* moves{nullptr};
};

void check_registers(FpuSpinner* spinner) noexcept {
  const RegisterFile mine = make_pattern(spinner->seed);
  utils::NullLock lock;

  {
    utils::FpuGuard guard{lock};
    load_registers(mine);
  }

  std::uint32_t last_cpu_seen = hw::percpu::id();

  for (std::uint32_t round = 0; round < spinner->rounds; ++round) {
    spin_for(Duration::microseconds(50));

    RegisterFile found{};
    {
      utils::FpuGuard guard{lock};
      store_registers(found);
    }

    if (!(found == mine)) {
      spinner->mismatches->fetch_add(1, std::memory_order_relaxed);
    }

    if (const std::uint32_t cpu = hw::percpu::id(); cpu != last_cpu_seen) {
      last_cpu_seen = cpu;
      spinner->moves->fetch_add(1, std::memory_order_relaxed);
    }
  }

  spinner->finished->fetch_add(1, std::memory_order_acq_rel);
}

constinit std::atomic<std::uint32_t> g_all_cpu_counter{0};
}  // namespace

KTEST_ALL_CPUS(Scheduler, every_cpu_has_its_queue_in_its_own_area, cpu) {
  RunQueue& rq = hw::percpu::runqueue();

  KEXPECT(&rq == &Scheduler::cpu_runqueue(cpu));
  KEXPECT_EQ(rq.cpu_id(), cpu);

  const auto* area = hw::percpu::self();
  const auto address = reinterpret_cast<std::uintptr_t>(&rq);
  KEXPECT_GE(address, reinterpret_cast<std::uintptr_t>(area));
  KEXPECT_LT(address, reinterpret_cast<std::uintptr_t>(area) + sizeof(hw::PerCpu));

  const std::uint32_t node = hw::percpu::numa_node_id();
  KEXPECT(rq.domain() == &Scheduler::numa_domain(node));
  KREQUIRE(hw::percpu::self()->numa_node != nullptr);
  KEXPECT(hw::percpu::self()->numa_node == smp::node_area(node));
  KEXPECT_EQ(hw::percpu::self()->numa_node->numa_id, node);
  KEXPECT_EQ(rq.domain()->cpu_at(rq.slot()), cpu);
  KEXPECT(rq.idle_thread() != nullptr);
  KEXPECT(rq.idle_thread()->is_idle());
}

KTEST(Scheduler, every_cpu_is_in_exactly_one_domain) {
  std::uint32_t total = 0;
  for (std::uint32_t node = 0; node < smp::node_count(); ++node) {
    total += Scheduler::numa_domain(node).cpu_count();
  }

  KEXPECT_EQ(total, smp::possible_cpus());
}

KTEST(Scheduler, refuses_what_it_cannot_run) {
  KEXPECT(Scheduler::enqueue(nullptr).error() == Error::InvalidArgument);
  KEXPECT(Scheduler::enqueue(sched::current()).error() == Error::InvalidArgument);

  const auto created = Thread::create_kernel(
      "offline", +[](void*) noexcept {}, nullptr, pinned_to(smp::possible_cpus() + 1));
  KREQUIRE(created.has_value());

  KEXPECT(Scheduler::enqueue(*created).error() == Error::InvalidState);
  KEXPECT(!(*created)->on_runqueue());
  KEXPECT(!(*created)->flags().get_scheduled());
  KEXPECT(Thread::destroy(*created).has_value());
}

// Right after the first enqueue is the interesting moment: another CPU may be picking the thread,
// in which case it is neither on a queue nor running. Over and over, to land in that window.
KTEST(Scheduler, a_thread_that_was_just_handed_over_cannot_be_handed_over_again) {
  for (std::uint32_t round = 0; round < 2000; ++round) {
    Counter counter;
    const auto created = Thread::spawn_kernel("race", &count_once, &counter);
    KREQUIRE(created.has_value());

    KEXPECT(Scheduler::enqueue(*created).has_value());
    const auto again = Scheduler::enqueue(*created);
    KEXPECT(!again.has_value());

    KEXPECT(wait_for([&] { return counter.done.load(std::memory_order_acquire) == 1; }));
  }
}

KTEST(Scheduler, a_thread_cannot_be_handed_over_twice) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes a CPU to run on and another to watch it");
  }

  Parked parked;
  const auto created = Thread::spawn_kernel("twice", &park, &parked, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());

  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(Scheduler::enqueue(*created).error() == Error::InvalidState);

  KEXPECT(wait_for([&] { return parked.running.load(std::memory_order_acquire); }));
  KEXPECT(Scheduler::enqueue(*created).error() == Error::InvalidState);

  parked.released.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return parked.finished.load(std::memory_order_acquire); }));
}

KTEST(Scheduler, runs_a_thread_and_cleans_up_after_it) {
  Counter counter;
  const auto created = Thread::spawn_kernel("once", &count_once, &counter);
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return counter.done.load(std::memory_order_acquire) == 1; }));
}

KTEST(Scheduler, a_new_thread_starts_with_a_clean_slate) {
  FirstLook look;

  const auto created = Thread::spawn_kernel(
      "first-look",
      +[](FirstLook* result) noexcept {
        result->preempt_count = hw::percpu::preempt_count();
        result->interrupts_on = hw::irq::is_enabled();

        utils::IrqDisableGuard guard;
        RunQueue& rq = hw::percpu::runqueue();
        rq.lock();
        rq.unlock();
        result->lock_is_free = true;
        result->done.store(true, std::memory_order_release);
      },
      &look, pinned_to(0));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return look.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(look.preempt_count, 0u);
  KEXPECT(look.interrupts_on);
  KEXPECT(look.lock_is_free);
}

KTEST(Scheduler, switching_leaves_the_preemption_count_where_it_was) {
  const std::uint8_t before = hw::percpu::preempt_count();

  Counter counter;
  const auto created = Thread::spawn_kernel("count", &count_once, &counter, pinned_to(0));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return counter.done.load(std::memory_order_acquire) == 1; }));

  Scheduler::yield();
  KEXPECT_EQ(hw::percpu::preempt_count(), before);
  KEXPECT(hw::irq::is_enabled());
}

KTEST(Scheduler, an_exit_hook_still_runs) {
  struct Hook {
    std::atomic<ThreadState> state{ThreadState::Created};
    std::atomic<bool> done{false};
  } hook;

  const auto created = Thread::create_kernel("hook", +[](void*) noexcept {});
  KREQUIRE(created.has_value());

  (*created)->set_exit_hook(
      [](Thread* self, void* cookie) noexcept {
        auto* result = static_cast<Hook*>(cookie);
        result->state.store(self->state(), std::memory_order_relaxed);
        result->done.store(true, std::memory_order_release);
      },
      &hook);

  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return hook.done.load(std::memory_order_acquire); }));
  KEXPECT(hook.state.load(std::memory_order_relaxed) == ThreadState::Dead);
}

KTEST(Scheduler, a_pinned_thread_stays_where_it_is_put) {
  for (std::uint32_t cpu = 0; cpu < smp::online_cpus(); ++cpu) {
    struct Report {
      std::uint32_t cpu;
      std::atomic<std::uint32_t> strayed{0};
      std::atomic<bool> done{false};
    } report{.cpu = cpu};

    const auto created = Thread::spawn_kernel(
        "pinned",
        +[](Report* result) noexcept {
          for (std::uint32_t round = 0; round < 100; ++round) {
            if (hw::percpu::id() != result->cpu) {
              result->strayed.fetch_add(1, std::memory_order_relaxed);
            }

            Scheduler::yield();
          }

          result->done.store(true, std::memory_order_release);
        },
        &report, pinned_to(cpu));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());

    KEXPECT(wait_for([&] { return report.done.load(std::memory_order_acquire); }));
    KEXPECT_EQ(report.strayed.load(std::memory_order_relaxed), 0u);
  }
}

KTEST(Scheduler, threads_take_turns_when_they_yield) {
  struct Turns {
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> finished{0};
    std::atomic<std::uint32_t> out_of_turn{0};
  } turns;

  struct Taker {
    Turns* turns;
    std::uint32_t index;
  };

  constexpr std::uint32_t THREADS = 3;
  constexpr std::uint32_t ROUNDS = 200;

  std::array<Taker, THREADS> takers{};
  for (std::uint32_t i = 0; i < THREADS; ++i) {
    takers[i] = {.turns = &turns, .index = i};

    const auto created = Thread::spawn_kernel(
        "turns",
        +[](Taker* me) noexcept {
          for (std::uint32_t round = 0; round < ROUNDS; ++round) {
            // Each waits for the one before it, so this only finishes if everyone gets a go.
            while (me->turns->next.load(std::memory_order_acquire) % THREADS != me->index) {
              Scheduler::yield();
            }

            me->turns->next.fetch_add(1, std::memory_order_release);
            Scheduler::yield();
          }

          me->turns->finished.fetch_add(1, std::memory_order_release);
        },
        &takers[i], pinned_to(last_cpu()));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return turns.finished.load(std::memory_order_acquire) == THREADS; }));
  KEXPECT_EQ(turns.next.load(std::memory_order_acquire), THREADS * ROUNDS);
}

KTEST(Scheduler, a_blocked_thread_stays_off_the_cpu_until_it_is_woken) {
  Sleeper sleeper;
  const auto created =
      Thread::spawn_kernel("sleeper", &sleep_until_woken, &sleeper, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] {
    Thread* self = sleeper.self.load(std::memory_order_acquire);
    return self != nullptr && self->state() == ThreadState::Blocked &&
           Scheduler::cpu_runqueue(last_cpu()).current() != self;
  }));

  // Has to be off the queue too, and to have stopped costing anything.
  Thread* self = sleeper.self.load(std::memory_order_acquire);
  KEXPECT(!self->on_runqueue());
  KEXPECT_EQ(sleeper.wakeups_seen.load(std::memory_order_acquire), 0u);

  sleeper.wakeups_wanted.store(1, std::memory_order_release);
  KEXPECT(Scheduler::wake(self));

  KEXPECT(wait_for([&] { return sleeper.wakeups_seen.load(std::memory_order_acquire) == 1; }));
}

KTEST(Scheduler, waking_a_thread_that_is_not_blocked_does_nothing) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes a CPU to run on and another to watch it");
  }

  Parked parked;
  const auto created = Thread::spawn_kernel("awake", &park, &parked, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());

  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return parked.running.load(std::memory_order_acquire); }));

  KEXPECT(!Scheduler::wake(*created));

  parked.released.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return parked.finished.load(std::memory_order_acquire); }));
}

// The classic: the wakeup arrives after the thread has decided to wait, but before it has left.
KTEST(Scheduler, a_wakeup_that_comes_early_is_not_lost) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes a CPU to run on and another to watch it");
  }

  struct Race {
    std::atomic<Thread*> self{nullptr};
    std::atomic<bool> go_on{false};
    std::atomic<bool> woken{false};
    std::atomic<bool> finished{false};
    std::atomic<ThreadState> state_after{ThreadState::Created};
  } race;

  const auto created = Thread::spawn_kernel(
      "early",
      +[](Race* r) noexcept {
        Scheduler::prepare_to_block();
        r->self.store(sched::current(), std::memory_order_release);

        // The wake() happens right here.
        while (!r->go_on.load(std::memory_order_acquire)) {
          hw::pause();
        }

        r->state_after.store(sched::current()->state(), std::memory_order_relaxed);
        Scheduler::schedule();
        r->finished.store(true, std::memory_order_release);
      },
      &race, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return race.self.load(std::memory_order_acquire) != nullptr; }));

  KEXPECT(Scheduler::wake(race.self.load(std::memory_order_acquire)));
  race.go_on.store(true, std::memory_order_release);

  KEXPECT(wait_for([&] { return race.finished.load(std::memory_order_acquire); }));
  KEXPECT(race.state_after.load(std::memory_order_relaxed) == ThreadState::Running);
}

KTEST(Scheduler, blocking_and_waking_on_one_cpu) {
  static Baton baton;
  baton.players[0].store(nullptr, std::memory_order_relaxed);
  baton.players[1].store(nullptr, std::memory_order_relaxed);
  baton.turn.store(0, std::memory_order_relaxed);
  baton.finished.store(0, std::memory_order_relaxed);
  baton.rounds = 2000;

  static std::array<Player, 2> players{Player{&baton, 0}, Player{&baton, 1}};
  for (Player& player : players) {
    const auto created = Thread::spawn_kernel("baton", &play, &player, pinned_to(last_cpu()));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return baton.finished.load(std::memory_order_acquire) == 2; }));
}

// Every wakeup here races a thread going to sleep on another CPU, thousands of times in a row. One
// lost wakeup anywhere and neither player gets any further.
KTEST(Scheduler, blocking_and_waking_across_cpus) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  static Baton baton;
  baton.players[0].store(nullptr, std::memory_order_relaxed);
  baton.players[1].store(nullptr, std::memory_order_relaxed);
  baton.turn.store(0, std::memory_order_relaxed);
  baton.finished.store(0, std::memory_order_relaxed);
  baton.rounds = 20000;

  static std::array<Player, 2> players{Player{&baton, 0}, Player{&baton, 1}};
  std::array<std::uint32_t, 2> cpus{last_cpu(), last_cpu() - 1};

  // The BSP runs this test, so it has to keep yielding for the player it might get.
  for (std::size_t i = 0; i < 2; ++i) {
    const auto created = Thread::spawn_kernel("baton", &play, &players[i], pinned_to(cpus[i]));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return baton.finished.load(std::memory_order_acquire) == 2; }));
}

KTEST(Scheduler, unpinned_threads_get_around_too) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  static Baton baton;
  baton.players[0].store(nullptr, std::memory_order_relaxed);
  baton.players[1].store(nullptr, std::memory_order_relaxed);
  baton.turn.store(0, std::memory_order_relaxed);
  baton.finished.store(0, std::memory_order_relaxed);
  baton.rounds = 20000;

  static std::array<Player, 2> players{Player{&baton, 0}, Player{&baton, 1}};
  for (Player& player : players) {
    const auto created = Thread::spawn_kernel("baton", &play, &player);
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return baton.finished.load(std::memory_order_acquire) == 2; }));
}

KTEST(Scheduler, a_flood_of_threads_all_get_run_and_cleaned_up) {
  Counter counter;
  constexpr std::uint32_t THREADS = 1500;

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    const auto created = Thread::spawn_kernel("flood", &count_once, &counter);
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());

    // Now and then, or this CPU's dead list has all of them at once.
    if (i % 32 == 0) {
      Scheduler::yield();
    }
  }

  KEXPECT(wait_for([&] { return counter.done.load(std::memory_order_acquire) == THREADS; }));
}

KTEST_ALL_CPUS(Scheduler, every_cpu_can_start_threads_at_once, cpu) {
  if (cpu == 0) {
    g_all_cpu_counter.store(0, std::memory_order_relaxed);
  }

  sync_cpus();

  constexpr std::uint32_t THREADS = 100;
  for (std::uint32_t i = 0; i < THREADS; ++i) {
    const auto created = Thread::spawn_kernel(
        "many",
        +[](std::atomic<std::uint32_t>* count) noexcept {
          count->fetch_add(1, std::memory_order_acq_rel);
        },
        const_cast<std::atomic<std::uint32_t>*>(&g_all_cpu_counter));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  const std::uint32_t expected = THREADS * cpu_count();
  KEXPECT(wait_for([&] { return g_all_cpu_counter.load(std::memory_order_acquire) == expected; }));

  sync_cpus();
}

KTEST(Scheduler, threads_spread_out_over_the_idle_cpus) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  // Whoever just finished a test may need a moment to get back to being idle.
  const std::uint32_t threads = std::min(smp::online_cpus() - 1, 64u);
  KEXPECT(wait_for([&] {
    std::uint32_t idle = 0;
    for (std::uint32_t node = 0; node < smp::node_count(); ++node) {
      idle += Scheduler::numa_domain(node).idle_cpu_count();
    }

    return idle >= threads;
  }));

  // One per idle CPU, each staying until all of them are running: they can only all get there if
  // they are on different CPUs. The time limit is there because a thread that ended up on this
  // CPU would otherwise be waiting for this code, which only runs when nothing else can.
  struct Crowd {
    std::atomic<std::uint32_t> arrived{0};
    std::atomic<std::uint64_t> cpus{0};
    std::atomic<std::uint32_t> finished{0};
    std::uint32_t expected{0};
    klib::TimePoint give_up_at{};
  } crowd{.expected = threads, .give_up_at = drivers::SystemClock::now() + Duration::seconds(10)};

  for (std::uint32_t i = 0; i < threads; ++i) {
    const auto created = Thread::spawn_kernel(
        "crowd",
        +[](Crowd* c) noexcept {
          c->cpus.fetch_or(1ul << (hw::percpu::id() % 64), std::memory_order_relaxed);
          c->arrived.fetch_add(1, std::memory_order_acq_rel);

          while (c->arrived.load(std::memory_order_acquire) != c->expected &&
                 drivers::SystemClock::now() < c->give_up_at) {
            hw::pause();
          }

          c->finished.fetch_add(1, std::memory_order_acq_rel);
        },
        &crowd);
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return crowd.finished.load(std::memory_order_acquire) == threads; }));

  // Not every one of them: which CPU is idle is only ever a hint, and on a host with fewer CPUs
  // than this guest, one that was just claimed can stay out of action for a good while. Piling
  // up is what must not happen.
  KEXPECT_GE(std::popcount(crowd.cpus.load(std::memory_order_acquire)),
             static_cast<int>((threads + 1) / 2));
}

KTEST(Scheduler, an_idle_cpu_takes_work_from_a_busy_one) {
  if (smp::online_cpus() < 3) {
    KTEST_SKIP("takes three CPUs: the test, one busy, one idle");
  }

  const std::uint32_t busy_cpu = last_cpu();

  // Real-time, so that the thread below has no way of getting this CPU: it can only leave.
  Parked occupant;
  const auto holder = Thread::spawn_kernel(
      "occupant", &park, &occupant,
      {.priority = PriorityClass::RealTime, .preferred_cpu = busy_cpu, .pinned = true});
  KREQUIRE(holder.has_value());
  KEXPECT(Scheduler::enqueue(*holder).has_value());
  KEXPECT(wait_for([&] { return occupant.running.load(std::memory_order_acquire); }));

  // Straight onto a queue, around everything that would have sent it to an idle CPU to begin with.
  Parked victim;
  const auto waiting = Thread::spawn_kernel("victim", &park, &victim);
  KREQUIRE(waiting.has_value());

  (*waiting)->set_cpu_id(busy_cpu);
  KEXPECT((*waiting)->try_hand_to_scheduler());

  RunQueue& rq = Scheduler::cpu_runqueue(busy_cpu);
  {
    utils::IrqSaveGuard guard{rq};
    (void)rq.enqueue_locked(**waiting);
  }

  // Everyone else is asleep, and nobody told them. The busy CPU's next interrupt does.
  hw::ipi::send_reschedule(busy_cpu);

  KEXPECT(wait_for([&] { return victim.running.load(std::memory_order_acquire); }));
  KEXPECT_NE(victim.cpu.load(std::memory_order_acquire), busy_cpu);

  victim.released.store(true, std::memory_order_release);
  occupant.released.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] {
    return victim.finished.load(std::memory_order_acquire) &&
           occupant.finished.load(std::memory_order_acquire);
  }));
}

KTEST(Scheduler, the_timer_takes_the_cpu_from_a_thread_that_never_gives_it_up) {
  std::array<std::atomic<std::uint64_t>, 2> progress{};
  std::atomic<std::uint32_t> gave_up{0};
  std::atomic<std::uint32_t> finished{0};
  std::array<TurnTaker, 2> takers{};

  for (std::uint32_t i = 0; i < 2; ++i) {
    takers[i] = {.progress = &progress,
                 .index = i,
                 .give_up_at = drivers::SystemClock::now() + Duration::seconds(5),
                 .gave_up = &gave_up,
                 .finished = &finished};

    const auto created =
        Thread::spawn_kernel("taker", &spin_until_both_ran, &takers[i], pinned_to(last_cpu()));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return finished.load(std::memory_order_acquire) == 2; }));
  KEXPECT_EQ(gave_up.load(std::memory_order_relaxed), 0u);
  KEXPECT_GT(takers[0].preempted + takers[1].preempted, 0u);
}

KTEST(Scheduler, cpu_time_goes_by_weight) {
  std::atomic<std::uint32_t> finished{0};
  std::array<Spinner, 2> spinners{};
  constexpr std::array<PriorityClass, 2> CLASSES{PriorityClass::Normal, PriorityClass::High};

  const klib::TimePoint until = drivers::SystemClock::now() + Duration::milliseconds(300);

  for (std::size_t i = 0; i < spinners.size(); ++i) {
    spinners[i].until = until;
    spinners[i].finished = &finished;

    const auto created =
        Thread::spawn_kernel("weighed", &spin, &spinners[i],
                             {.priority = CLASSES[i], .preferred_cpu = last_cpu(), .pinned = true});
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return finished.load(std::memory_order_acquire) == 2; }));

  // Four to one by the weights. Iterations are only a rough stand-in for time, and on a shared
  // host the rest of the machine gets a say too, so this is about direction and not decimals.
  const std::uint64_t normal = spinners[0].iterations.load(std::memory_order_relaxed);
  const std::uint64_t high = spinners[1].iterations.load(std::memory_order_relaxed);
  KEXPECT_GT(high, 2 * normal);
  KEXPECT_LT(high, 10 * normal);
}

// A CPU with one thread on it runs no slice timer. When a second one turns up, from another CPU,
// and has no claim on the CPU yet, then somebody has to tell that CPU to start one, or the first
// thread keeps it for as long as it pleases.
KTEST(Scheduler, a_thread_that_joins_a_busy_cpu_gets_a_turn) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  // A slice timer left over from an earlier test would do the job by accident.
  (void)wait_for([] { return false; }, Duration::milliseconds(20));

  struct Scene {
    std::atomic<bool> running{false};
    std::atomic<std::uint32_t> joined{0};
    std::atomic<bool> finished{false};
    bool gave_up{false};
    klib::TimePoint give_up_at{};
  } scene{.give_up_at = drivers::SystemClock::now() + Duration::seconds(5)};

  const auto hog = Thread::spawn_kernel(
      "alone",
      +[](Scene* s) noexcept {
        s->running.store(true, std::memory_order_release);

        while (s->joined.load(std::memory_order_acquire) == 0) {
          if (drivers::SystemClock::now() > s->give_up_at) {
            s->gave_up = true;
            break;
          }
        }

        s->finished.store(true, std::memory_order_release);
      },
      &scene, pinned_to(last_cpu()));
  KREQUIRE(hog.has_value());
  KEXPECT(Scheduler::enqueue(*hog).has_value());
  KEXPECT(wait_for([&] { return scene.running.load(std::memory_order_acquire); }));

  // A late deadline, so that it has no reason to take over right away.
  const auto joiner = Thread::spawn_kernel(
      "joiner",
      +[](std::atomic<std::uint32_t>* joined) noexcept {
        joined->store(1, std::memory_order_release);
      },
      &scene.joined,
      {.preferred_cpu = last_cpu(), .slice_ns = 8 * sched::DEFAULT_SLICE_NS, .pinned = true});
  KREQUIRE(joiner.has_value());
  KEXPECT(Scheduler::enqueue(*joiner).has_value());

  KEXPECT(wait_for([&] { return scene.finished.load(std::memory_order_acquire); }));
  KEXPECT(!scene.gave_up);
}

KTEST(Scheduler, a_real_time_thread_does_not_have_to_wait_for_a_slice) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  // Busy until the urgent thread has been: nothing that reaches a busy CPU from elsewhere will
  // get a turn before this is over, unless it takes the CPU.
  struct Hog {
    std::atomic<bool> running{false};
    std::atomic<std::uint32_t> urgent_ran{0};
    std::atomic<bool> finished{false};
    klib::TimePoint give_up_at{};
    bool gave_up{false};
  } hog{.give_up_at = drivers::SystemClock::now() + Duration::seconds(5)};

  const auto created = Thread::spawn_kernel(
      "hog",
      +[](Hog* h) noexcept {
        h->running.store(true, std::memory_order_release);
        while (h->urgent_ran.load(std::memory_order_acquire) == 0) {
          if (drivers::SystemClock::now() > h->give_up_at) {
            h->gave_up = true;
            break;
          }
        }

        h->finished.store(true, std::memory_order_release);
      },
      &hog, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return hog.running.load(std::memory_order_acquire); }));

  const auto urgent = Thread::spawn_kernel(
      "urgent",
      +[](std::atomic<std::uint32_t>* ran) noexcept { ran->store(1, std::memory_order_release); },
      &hog.urgent_ran,
      {.priority = PriorityClass::RealTime, .preferred_cpu = last_cpu(), .pinned = true});
  KREQUIRE(urgent.has_value());
  KEXPECT(Scheduler::enqueue(*urgent).has_value());

  KEXPECT(wait_for([&] { return hog.finished.load(std::memory_order_acquire); }));
  KEXPECT(!hog.gave_up);
}

// A thread that loses its CPU is not on the way to anywhere: it goes back on a queue, behind
// something that may never stop, while other CPUs sleep with nothing to do and no reason to look.
//
// Nothing else may happen here, or it would wake them up by accident: the thread that takes over is
// created in advance, as allocating a stack sends IPIs around, and everything is given time to go
// quiet first. With a slice this long, the slice timer is not coming to the rescue either.
KTEST(Scheduler, a_thread_that_loses_its_cpu_goes_to_an_idle_one) {
  if (smp::online_cpus() < 3) {
    KTEST_SKIP("takes three CPUs: the test, the thread's, and the one it moves to");
  }

  struct Scene {
    Thread* usurper{nullptr};
    std::uint32_t start_cpu{0};
    std::atomic<bool> moved{false};
    std::atomic<bool> done{false};
    std::atomic<bool> usurper_done{false};
    klib::TimePoint give_up_at{};
  } scene{.start_cpu = last_cpu(),
          .give_up_at = drivers::SystemClock::now() + Duration::seconds(3)};

  const auto usurper = Thread::spawn_kernel(
      "usurper",
      +[](Scene* s) noexcept {
        while (!s->moved.load(std::memory_order_acquire) &&
               drivers::SystemClock::now() < s->give_up_at) {
          hw::pause();
        }

        s->usurper_done.store(true, std::memory_order_release);
      },
      &scene,
      {.priority = PriorityClass::RealTime, .preferred_cpu = scene.start_cpu, .pinned = true});
  KREQUIRE(usurper.has_value());
  scene.usurper = *usurper;

  // Stays away from the test's CPU by asking for the last one, which should be idle by now.
  (void)wait_for([] { return false; }, Duration::milliseconds(50));

  const auto displaced = Thread::spawn_kernel(
      "displaced",
      +[](Scene* s) noexcept {
        spin_for(Duration::milliseconds(50));

        // Where it said it wanted to go is only a preference, so this goes by where it ended up.
        const std::uint32_t before = hw::percpu::id();
        s->usurper->set_cpu_id(before);

        if (Scheduler::enqueue(s->usurper).has_value()) {
          s->moved.store(hw::percpu::id() != before, std::memory_order_release);
        }

        s->done.store(true, std::memory_order_release);
      },
      &scene, {.preferred_cpu = scene.start_cpu, .slice_ns = 60'000'000'000});
  KREQUIRE(displaced.has_value());
  KEXPECT(Scheduler::enqueue(*displaced).has_value());

  KEXPECT(wait_for([&] {
    return scene.done.load(std::memory_order_acquire) &&
           scene.usurper_done.load(std::memory_order_acquire);
  }));
  KEXPECT(scene.moved.load(std::memory_order_acquire));
}

// An interrupt that wants the CPU back cannot have it while the thread holds a lock. The thread has
// to give way when it lets go: the next interrupt may be a whole slice away, or never come.
KTEST(Scheduler, a_thread_gives_way_as_soon_as_preemption_is_back_on) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  struct Scene {
    std::atomic<bool> holding{false};
    std::atomic<std::uint32_t> urgent_ran{0};
    std::atomic<bool> finished{false};
    std::uint32_t seen_on_release{0};
    bool request_arrived{false};
    klib::TimePoint give_up_at{};
  } scene{.give_up_at = drivers::SystemClock::now() + Duration::seconds(5)};

  const auto holder = Thread::spawn_kernel(
      "holder",
      +[](Scene* s) noexcept {
        hw::percpu::preempt_disable();
        s->holding.store(true, std::memory_order_release);

        // Until the interrupt has been and gone, unable to do anything about this thread.
        while ((hw::percpu::ast_flags() & hw::percpu::AST_NEED_RESCHED) == 0 &&
               drivers::SystemClock::now() < s->give_up_at) {
          hw::pause();
        }

        s->request_arrived = (hw::percpu::ast_flags() & hw::percpu::AST_NEED_RESCHED) != 0;

        hw::percpu::preempt_enable();
        s->seen_on_release = s->urgent_ran.load(std::memory_order_acquire);
        s->finished.store(true, std::memory_order_release);
      },
      &scene, pinned_to(last_cpu()));
  KREQUIRE(holder.has_value());
  KEXPECT(Scheduler::enqueue(*holder).has_value());
  KEXPECT(wait_for([&] { return scene.holding.load(std::memory_order_acquire); }));

  const auto urgent = Thread::spawn_kernel(
      "urgent",
      +[](std::atomic<std::uint32_t>* ran) noexcept { ran->store(1, std::memory_order_release); },
      &scene.urgent_ran,
      {.priority = PriorityClass::RealTime, .preferred_cpu = last_cpu(), .pinned = true});
  KREQUIRE(urgent.has_value());
  KEXPECT(Scheduler::enqueue(*urgent).has_value());

  KEXPECT(wait_for([&] { return scene.finished.load(std::memory_order_acquire); }));
  KEXPECT(scene.request_arrived);
  KEXPECT_EQ(scene.seen_on_release, 1u);
}

// The same with interrupts off too, the way every spinlock guard does it: interrupts have to be
// back on by the time the guard looks, or it finds it cannot switch, and the moment is gone.
KTEST(Scheduler, a_guard_that_held_interrupts_off_gives_way_as_well) {
  struct Scene {
    std::atomic<std::uint32_t> urgent_ran{0};
    std::uint32_t seen_on_release{0};
    std::atomic<bool> finished{false};
  } scene;

  const auto created = Thread::spawn_kernel(
      "starter",
      +[](Scene* s) noexcept {
        const auto urgent = Thread::spawn_kernel("urgent",
                                                 +[](std::atomic<std::uint32_t>* ran) noexcept {
                                                   ran->store(1, std::memory_order_release);
                                                 },
                                                 &s->urgent_ran,
                                                 {.priority = PriorityClass::RealTime,
                                                  .preferred_cpu = hw::percpu::id(),
                                                  .pinned = true});

        if (urgent) {
          utils::IrqDisableGuard guard;
          (void)Scheduler::enqueue(*urgent);
        }

        s->seen_on_release = s->urgent_ran.load(std::memory_order_acquire);
        s->finished.store(true, std::memory_order_release);
      },
      &scene, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return scene.finished.load(std::memory_order_acquire); }));
  KEXPECT_EQ(scene.seen_on_release, 1u);
}

// The other side of it: code that turned off interrupts by hand wants the CPU to itself, and
// neither a guard coming and going inside that, nor anything it wakes, gets to take it away.
KTEST(Scheduler, a_thread_with_interrupts_off_keeps_the_cpu) {
  struct Scene {
    std::atomic<std::uint32_t> urgent_ran{0};
    std::uint32_t seen_with_interrupts_off{1};
    std::uint32_t seen_afterwards{0};
    std::atomic<bool> finished{false};
  } scene;

  const auto created = Thread::spawn_kernel(
      "keeper",
      +[](Scene* s) noexcept {
        const auto urgent = Thread::spawn_kernel("urgent",
                                                 +[](std::atomic<std::uint32_t>* ran) noexcept {
                                                   ran->store(1, std::memory_order_release);
                                                 },
                                                 &s->urgent_ran,
                                                 {.priority = PriorityClass::RealTime,
                                                  .preferred_cpu = hw::percpu::id(),
                                                  .pinned = true});

        if (urgent) {
          hw::irq::disable();
          (void)Scheduler::enqueue(*urgent);

          {
            utils::IrqDisableGuard guard;
          }

          s->seen_with_interrupts_off = s->urgent_ran.load(std::memory_order_acquire);
          hw::irq::enable();

          // Still pending, so the next chance it gets is this one.
          hw::percpu::preempt_disable();
          hw::percpu::preempt_enable();
          s->seen_afterwards = s->urgent_ran.load(std::memory_order_acquire);
        }

        s->finished.store(true, std::memory_order_release);
      },
      &scene, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return scene.finished.load(std::memory_order_acquire); }));
  KEXPECT_EQ(scene.seen_with_interrupts_off, 0u);
  KEXPECT_EQ(scene.seen_afterwards, 1u);
}

// The last thread on a CPU has nothing but the idle thread running after it, and that switch has to
// hand it to the reaper like any other. Otherwise the stack of a thread that is long gone sits on a
// sleeping CPU.
KTEST(Scheduler, the_last_thread_on_a_cpu_does_not_stay_behind_when_it_goes_idle) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  Counter counter;
  const auto created = Thread::spawn_kernel("last", &count_once, &counter, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return counter.done.load(std::memory_order_acquire) == 1; }));

  const RunQueue& rq = Scheduler::cpu_runqueue(last_cpu());
  KEXPECT(wait_for([&] { return !rq.has_dead_threads(); }));
}

KTEST(Scheduler, a_thread_that_starts_something_urgent_gets_out_of_the_way) {
  struct Report {
    std::atomic<std::uint32_t> urgent_ran{0};
    std::uint32_t seen_after_enqueue{0};
    std::atomic<bool> done{false};
  } report;

  const auto created = Thread::spawn_kernel(
      "starter",
      +[](Report* r) noexcept {
        const auto urgent = Thread::spawn_kernel("urgent",
                                                 +[](std::atomic<std::uint32_t>* ran) noexcept {
                                                   ran->store(1, std::memory_order_release);
                                                 },
                                                 &r->urgent_ran,
                                                 {.priority = PriorityClass::RealTime,
                                                  .preferred_cpu = hw::percpu::id(),
                                                  .pinned = true});

        if (urgent && Scheduler::enqueue(*urgent).has_value()) {
          r->seen_after_enqueue = r->urgent_ran.load(std::memory_order_acquire);
        }

        r->done.store(true, std::memory_order_release);
      },
      &report, pinned_to(last_cpu()));
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return report.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(report.seen_after_enqueue, 1u);
}

// A kernel thread's FPU registers are saved at the switch and nowhere else, so this is the one
// thing that goes wrong if a preempted thread is treated any differently from one that yielded.
KTEST(Scheduler, fpu_registers_survive_being_preempted) {
  std::atomic<std::uint32_t> finished{0};
  std::atomic<std::uint32_t> mismatches{0};
  std::atomic<std::uint32_t> moves{0};
  std::array<FpuSpinner, 3> spinners{};

  for (std::size_t i = 0; i < spinners.size(); ++i) {
    spinners[i] = {.seed = 700 + i,
                   .rounds = 300,
                   .finished = &finished,
                   .mismatches = &mismatches,
                   .moves = &moves};

    const auto created =
        Thread::spawn_kernel("fpu", &check_registers, &spinners[i], pinned_to(last_cpu()));
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return finished.load(std::memory_order_acquire) == spinners.size(); }));
  KEXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u);
  KEXPECT_EQ(moves.load(std::memory_order_relaxed), 0u);
}

KTEST(Scheduler, fpu_registers_survive_a_crowd) {
  std::atomic<std::uint32_t> finished{0};
  std::atomic<std::uint32_t> mismatches{0};
  std::atomic<std::uint32_t> moves{0};
  static std::array<FpuSpinner, 64> spinners{};

  // Twice as many threads as CPUs and unequal amounts of work, so they get preempted, wait their
  // turn and are taken by whichever CPU runs out first.
  const std::uint32_t threads =
      std::min(2 * smp::online_cpus(), static_cast<std::uint32_t>(spinners.size()));

  for (std::uint32_t i = 0; i < threads; ++i) {
    spinners[i] = {.seed = 900 + i,
                   .rounds = 100 + 50 * i,
                   .finished = &finished,
                   .mismatches = &mismatches,
                   .moves = &moves};

    const auto created = Thread::spawn_kernel("fpu", &check_registers, &spinners[i]);
    KREQUIRE(created.has_value());
    KEXPECT(Scheduler::enqueue(*created).has_value());
  }

  KEXPECT(wait_for([&] { return finished.load(std::memory_order_acquire) == threads; }));
  KEXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0u);
}

// The one thing a thread cannot do by itself is leave a CPU in the middle of what it was doing,
// with its registers still on that CPU. This arranges for exactly that: a real-time thread takes
// over from it, it ends up on a queue, and an idle CPU steals it from there.
KTEST(Scheduler, fpu_registers_survive_being_stolen) {
  if (smp::online_cpus() < 3) {
    KTEST_SKIP("takes three CPUs: the test, the victim's, and the thief's");
  }

  struct Victim {
    std::atomic<bool> running{false};
    std::atomic<std::uint32_t> first_cpu{~0u};
    std::atomic<std::uint32_t> last_cpu{~0u};
    std::atomic<bool> intact{false};
    std::atomic<bool> finished{false};
    klib::TimePoint give_up_at{};
  } victim{.give_up_at = drivers::SystemClock::now() + Duration::seconds(10)};

  const auto created = Thread::spawn_kernel(
      "victim",
      +[](Victim* v) noexcept {
        const RegisterFile mine = make_pattern(1234);
        utils::NullLock lock;

        {
          utils::FpuGuard guard{lock};
          load_registers(mine);
        }

        const std::uint32_t start = hw::percpu::id();
        v->first_cpu.store(start, std::memory_order_release);
        v->running.store(true, std::memory_order_release);

        while (hw::percpu::id() == start && drivers::SystemClock::now() < v->give_up_at) {
          hw::pause();
        }

        RegisterFile found{};
        {
          utils::FpuGuard guard{lock};
          store_registers(found);
        }

        v->last_cpu.store(hw::percpu::id(), std::memory_order_release);
        v->intact.store(found == mine, std::memory_order_release);
        v->finished.store(true, std::memory_order_release);
      },
      &victim);
  KREQUIRE(created.has_value());
  KEXPECT(Scheduler::enqueue(*created).has_value());
  KEXPECT(wait_for([&] { return victim.running.load(std::memory_order_acquire); }));

  Parked occupant;
  const auto usurper =
      Thread::spawn_kernel("usurper", &park, &occupant,
                           {.priority = PriorityClass::RealTime,
                            .preferred_cpu = victim.first_cpu.load(std::memory_order_acquire),
                            .pinned = true});
  KREQUIRE(usurper.has_value());
  KEXPECT(Scheduler::enqueue(*usurper).has_value());

  KEXPECT(wait_for([&] { return victim.finished.load(std::memory_order_acquire); }));
  occupant.released.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return occupant.finished.load(std::memory_order_acquire); }));

  KEXPECT_NE(victim.last_cpu.load(std::memory_order_acquire),
             victim.first_cpu.load(std::memory_order_acquire));
  KEXPECT(victim.intact.load(std::memory_order_acquire));
}

KTEST_LAST(Scheduler, the_queues_are_empty_when_the_tests_are_over) {
  (void)wait_for([] { return false; }, Duration::milliseconds(20));

  for (std::uint32_t cpu = 0; cpu < smp::online_cpus(); ++cpu) {
    KEXPECT_EQ(Scheduler::cpu_runqueue(cpu).waiting(), 0u);
  }
}

// Nothing wakes a CPU with nothing to do. A periodic tick would show up here at once.
KTEST(Scheduler, an_idle_cpu_gets_no_timer_interrupts) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("the test itself is keeping the only CPU busy");
  }

  const std::uint32_t cpu = last_cpu();
  const hw::timer::TimerQueue& timers = hw::percpu::cpu_area(cpu)->timers;

  // Give it a moment to finish whatever the last test left there.
  spin_for(Duration::milliseconds(10));
  const hw::timer::TimerStats before = timers.stats();

  spin_for(Duration::milliseconds(30));
  const hw::timer::TimerStats after = timers.stats();

  KEXPECT_EQ(after.expired, before.expired);
  KEXPECT_EQ(after.idle_wakeups, before.idle_wakeups);
  KEXPECT_EQ(after.hardware_arms, before.hardware_arms);
}

KTEST(Scheduler, a_thread_alone_on_a_cpu_is_left_alone) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("the test itself is keeping the only CPU busy");
  }

  const std::uint32_t cpu = last_cpu();
  const hw::timer::TimerQueue& timers = hw::percpu::cpu_area(cpu)->timers;

  Parked parked;
  const auto created = Thread::spawn_kernel("alone", &park, &parked, pinned_to(cpu));
  KREQUIRE(created.has_value());
  KREQUIRE(Scheduler::enqueue(*created).has_value());
  KREQUIRE(wait_for([&] { return parked.running.load(std::memory_order_acquire); }));

  spin_for(Duration::milliseconds(5));
  const std::uint64_t before = timers.stats().expired;

  spin_for(Duration::milliseconds(30));
  const std::uint64_t after = timers.stats().expired;

  parked.released.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return parked.finished.load(std::memory_order_acquire); }));

  KEXPECT_EQ(after, before);
}

namespace {
// Runs flat out, noting how long it gets to run in one go.
struct Stint {
  std::atomic<bool>* stop{nullptr};
  std::atomic<std::uint32_t>* running{nullptr};
  std::array<std::uint64_t, 64> stints_ns{};
  std::uint32_t count{0};
};

void measure_stints(Stint* stint) noexcept {
  // Anything longer than this between two looks at the clock is the thread being away.
  constexpr std::uint64_t AWAY_NS = 50'000;

  std::uint64_t began = drivers::SystemClock::now().raw_ns();
  std::uint64_t last = began;

  while (!stint->stop->load(std::memory_order_acquire)) {
    const std::uint64_t now = drivers::SystemClock::now().raw_ns();

    if (now - last > AWAY_NS) {
      if (stint->count < stint->stints_ns.size()) {
        stint->stints_ns[stint->count++] = last - began;
      }

      began = now;
    }

    last = now;
  }

  stint->running->fetch_sub(1, std::memory_order_acq_rel);
}
}  // namespace

// A millisecond each, with room for what a VM does to timing: a host that takes the CPU away for a
// moment cuts a stint in two or makes it look longer. The median is what counts.
KTEST(Scheduler, a_slice_lasts_as_long_as_it_says) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("the test needs to get a word in");
  }

  static constexpr std::uint64_t SLICE_NS = 1'000'000;

  const std::uint32_t cpu = last_cpu();
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> running{2};
  std::array<Stint, 2> stints{Stint{.stop = &stop, .running = &running},
                              Stint{.stop = &stop, .running = &running}};

  ThreadCreateOptions options = pinned_to(cpu);
  options.slice_ns = SLICE_NS;

  for (Stint& stint : stints) {
    const auto created = Thread::spawn_kernel("stint", &measure_stints, &stint, options);
    KREQUIRE(created.has_value());
    KREQUIRE(Scheduler::enqueue(*created).has_value());
  }

  spin_for(Duration::milliseconds(100));
  stop.store(true, std::memory_order_release);
  KREQUIRE(wait_for([&] { return running.load(std::memory_order_acquire) == 0; }));

  for (Stint& stint : stints) {
    KREQUIRE_GT(stint.count, 4u);

    // The first one starts somewhere in the middle of things.
    const std::span measured{stint.stints_ns.data() + 1, stint.count - 1};
    std::ranges::sort(measured);

    const std::uint64_t median = measured[measured.size() / 2];
    KEXPECT_GE(median, SLICE_NS / 4);
    KEXPECT_LE(median, SLICE_NS * 4);
  }
}

// The last thread takes its slice timer's deadline with it. Going off anyway would wake a CPU with
// nothing to do.
KTEST(Scheduler, going_idle_stops_the_slice_timer) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("the test itself is keeping the only CPU busy");
  }

  const std::uint32_t cpu = last_cpu();
  const hw::timer::TimerQueue& timers = hw::percpu::cpu_area(cpu)->timers;
  const hw::timer::Timer& slice_timer = Scheduler::cpu_runqueue(cpu).slice_timer();

  // Two threads take turns, which is what starts it. Their slices are long enough for the
  // timer to still be waiting when they are done.
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> running{2};
  std::array<Stint, 2> stints{Stint{.stop = &stop, .running = &running},
                              Stint{.stop = &stop, .running = &running}};

  ThreadCreateOptions options = pinned_to(cpu);
  options.slice_ns = Duration::milliseconds(50).to_ns();

  for (Stint& stint : stints) {
    const auto created = Thread::spawn_kernel("stint", &measure_stints, &stint, options);
    KREQUIRE(created.has_value());
    KREQUIRE(Scheduler::enqueue(*created).has_value());
  }

  KREQUIRE(wait_for([&] { return slice_timer.is_pending(); }));

  stop.store(true, std::memory_order_release);
  KREQUIRE(wait_for([&] { return running.load(std::memory_order_acquire) == 0; }));

  KEXPECT(wait_for([&] { return !slice_timer.is_pending(); }, Duration::milliseconds(20)));

  const std::uint64_t wakeups = timers.stats().idle_wakeups;
  spin_for(Duration::milliseconds(60));
  KEXPECT_EQ(timers.stats().idle_wakeups, wakeups);
}

}  // namespace kernel::testing
