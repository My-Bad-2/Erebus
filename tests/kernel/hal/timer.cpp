#include "hal/timer/timer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "sched/scheduler.hpp"
#include "utils/logger.hpp"

namespace kernel::testing {
namespace {
namespace smp = hw::smp;
using namespace klib::literals;
using drivers::SystemClock;
using hw::timer::LatencyEstimator;
using hw::timer::Timer;
using hw::timer::TimerAction;
using hw::timer::TimerPrecision;
using hw::timer::TimerQueue;
using klib::Duration;
using klib::TimePoint;

[[nodiscard]] constexpr TimePoint at(const std::uint64_t ns) noexcept { return TimePoint{ns}; }

// For the queues without hardware, which go by whatever time they are told.
struct Log {
  std::array<std::uint32_t, 128> order{};
  std::uint32_t count{0};
};

struct Logged {
  Log* log{nullptr};
  std::uint32_t id{0};
  Timer timer{};

  Logged() noexcept = default;

  void bind(Log& target, const std::uint32_t tag) noexcept {
    log = &target;
    id = tag;
    timer.set_callback(
        [](Timer&, void* context) noexcept {
          auto* self = static_cast<Logged*>(context);
          self->log->order[self->log->count++] = self->id;
          return TimerAction::Done;
        },
        this);
  }
};

// Tells what time it was when the callback ran.
struct Probe {
  std::atomic<std::uint64_t> fired_ns{0};
  std::atomic<std::uint32_t> count{0};
  std::atomic<std::uint32_t> cpu{~0u};
};

TimerAction probe_fired(Timer&, void* context) noexcept {
  auto* probe = static_cast<Probe*>(context);
  probe->fired_ns.store(SystemClock::now().raw_ns(), std::memory_order_relaxed);
  probe->cpu.store(hw::percpu::id(), std::memory_order_relaxed);
  probe->count.fetch_add(1, std::memory_order_release);
  return TimerAction::Done;
}

template <typename Pred>
[[nodiscard]] bool wait_for(Pred&& done, const Duration timeout = 10_s) noexcept {
  const TimePoint deadline = SystemClock::now() + timeout;
  while (!done()) {
    if (SystemClock::now() > deadline) {
      return false;
    }

    hw::cpu_relax();
  }

  return true;
}

// Sits out timers from earlier tests, which would otherwise show up in this one's numbers.
void settle() noexcept {
  const TimePoint end = SystemClock::now() + 5_ms;
  while (SystemClock::now() < end) {
    hw::pause();
  }
}

template <std::size_t N>
[[nodiscard]] std::uint64_t median(std::array<std::uint64_t, N>& samples) noexcept {
  std::ranges::sort(samples);
  return samples[N / 2];
}

constexpr std::uint32_t PERIODS = 500;
constexpr Duration PERIOD = 200_us;

Timer g_contested{};
std::atomic<std::uint32_t> g_contested_runs{0};
}  // namespace

// ---- Fake time: what the queue decides, with nothing else going on ------------------------------

KTEST(TimerQueue, nothing_runs_before_its_deadline) {
  TimerQueue queue;
  Log log;
  Logged timer;
  timer.bind(log, 1);

  KEXPECT(!queue.add(timer.timer, at(1000)));
  KEXPECT(timer.timer.is_pending());
  KEXPECT(queue.first() == &timer.timer);

  KEXPECT_EQ(queue.expire(999), 0u);
  KEXPECT_EQ(log.count, 0u);

  KEXPECT_EQ(queue.expire(1000), 1u);
  KEXPECT_EQ(log.count, 1u);
  KEXPECT(!timer.timer.is_pending());
  KEXPECT(queue.first() == nullptr);

  KEXPECT_EQ(queue.expire(5000), 0u);
}

KTEST(TimerQueue, timers_run_in_order_of_their_deadlines) {
  TimerQueue queue;
  Log log;
  std::array<Logged, 32> timers;

  for (std::uint32_t i = 0; i < timers.size(); ++i) {
    timers[i].bind(log, i);
    (void)queue.add(timers[i].timer, at(1000 + ((i * 13) % 32) * 10));
  }

  KEXPECT(queue.is_consistent());
  KEXPECT_EQ(queue.expire(1000 + 31 * 10), 32u);

  KREQUIRE_EQ(log.count, 32u);
  for (std::uint32_t i = 1; i < 32; ++i) {
    KEXPECT_LT((log.order[i - 1] * 13) % 32, (log.order[i] * 13) % 32);
  }
}

KTEST(TimerQueue, equal_deadlines_go_in_the_order_they_came) {
  TimerQueue queue;
  Log log;
  std::array<Logged, 16> timers;

  for (std::uint32_t i = 0; i < timers.size(); ++i) {
    timers[i].bind(log, i);
    (void)queue.add(timers[i].timer, at(500));
  }

  KEXPECT_EQ(queue.expire(500), 16u);
  for (std::uint32_t i = 0; i < 16; ++i) {
    KEXPECT_EQ(log.order[i], i);
  }
}

KTEST(TimerQueue, a_cancelled_timer_does_not_run) {
  TimerQueue queue;
  Log log;
  std::array<Logged, 3> timers;
  for (std::uint32_t i = 0; i < timers.size(); ++i) {
    timers[i].bind(log, i);
    (void)queue.add(timers[i].timer, at(100 * (i + 1)));
  }

  // The first, so that the next one has to take over.
  KEXPECT(timers[0].timer.cancel());
  KEXPECT(!timers[0].timer.cancel());
  KEXPECT(!timers[0].timer.is_pending());
  KEXPECT(queue.first() == &timers[1].timer);
  KEXPECT(queue.is_consistent());

  KEXPECT_EQ(queue.expire(300), 2u);
  KEXPECT_EQ(log.order[0], 1u);
  KEXPECT_EQ(log.order[1], 2u);
}

KTEST(TimerQueue, starting_a_waiting_timer_replaces_its_deadline) {
  TimerQueue queue;
  Log log;
  Logged timer;
  timer.bind(log, 7);

  (void)queue.add(timer.timer, at(1000));
  KEXPECT(queue.add(timer.timer, at(2000)));
  KEXPECT(queue.is_consistent());

  KEXPECT_EQ(queue.expire(1500), 0u);
  KEXPECT_EQ(queue.expire(2000), 1u);
}

KTEST(TimerQueue, a_timer_can_move_to_another_queue) {
  TimerQueue first;
  TimerQueue second;
  Log log;
  Logged timer;
  timer.bind(log, 1);

  (void)first.add(timer.timer, at(100));
  KEXPECT(second.add(timer.timer, at(100)));

  KEXPECT(first.first() == nullptr);
  KEXPECT(second.first() == &timer.timer);
  KEXPECT_EQ(first.expire(100), 0u);
  KEXPECT_EQ(second.expire(100), 1u);
}

// A timer with slack stays out of the hardware's way, and goes along when something else fires.
KTEST(TimerQueue, slack_lets_a_timer_share_an_interrupt) {
  TimerQueue queue;
  Log log;
  Logged relaxed;
  Logged strict;
  relaxed.bind(log, 1);
  strict.bind(log, 2);

  (void)queue.add(relaxed.timer, at(100), 50_ns);
  (void)queue.add(strict.timer, at(120));

  // What the hardware would wait for is the end of the slack, so the strict one is first.
  KEXPECT(queue.first() == &strict.timer);

  KEXPECT_EQ(queue.expire(120), 2u);
  KEXPECT_EQ(log.order[0], 2u);
  KEXPECT_EQ(log.order[1], 1u);
}

KTEST(TimerQueue, slack_never_means_early) {
  TimerQueue queue;
  Log log;
  Logged timer;
  timer.bind(log, 1);

  (void)queue.add(timer.timer, at(100), 1000_ns);

  KEXPECT_EQ(queue.expire(99), 0u);
  KEXPECT_EQ(queue.expire(100), 1u);
}

KTEST(TimerQueue, a_periodic_timer_keeps_to_its_grid) {
  struct Periodic {
    Timer timer{};
    std::uint64_t now{0};
    std::uint32_t runs{0};
    std::uint64_t last_periods{0};
    std::array<std::uint64_t, 8> deadlines{};
  } periodic;

  periodic.timer.set_callback(
      [](Timer& timer, void* context) noexcept {
        auto* self = static_cast<Periodic*>(context);
        self->deadlines[self->runs++] = timer.deadline().raw_ns();
        self->last_periods = timer.forward(100_ns, at(self->now));
        return self->runs < 8 ? TimerAction::Restart : TimerAction::Done;
      },
      &periodic);

  TimerQueue queue;
  (void)queue.add(periodic.timer, at(1000));

  // Always a little late, and differently each time. Nothing of that shows up in the deadlines.
  std::uint64_t now = 1000;
  for (std::uint32_t run = 0; run < 8; ++run) {
    now = 1000 + run * 100 + (run * 17) % 90;
    periodic.now = now;
    KEXPECT_EQ(queue.expire(now), 1u);
    KEXPECT_EQ(periodic.last_periods, 1u);
  }

  for (std::uint32_t run = 0; run < 8; ++run) {
    KEXPECT_EQ(periodic.deadlines[run], 1000u + run * 100);
  }

  KEXPECT(!periodic.timer.is_pending());
}

KTEST(TimerQueue, periods_that_were_missed_are_skipped_and_counted) {
  Timer timer{[](Timer&, void*) noexcept { return TimerAction::Done; }};

  TimerQueue queue;
  (void)queue.add(timer, at(1000));
  (void)queue.expire(1000);

  KEXPECT_EQ(timer.forward(100_ns, at(1350)), 4u);
  KEXPECT_EQ(timer.deadline().raw_ns(), 1400u);

  // On the deadline exactly is not in the future yet.
  KEXPECT_EQ(timer.forward(100_ns, at(1400)), 1u);
  KEXPECT_EQ(timer.deadline().raw_ns(), 1500u);

  // Before it, one period is all that is needed.
  KEXPECT_EQ(timer.forward(100_ns, at(1000)), 1u);
  KEXPECT_EQ(timer.deadline().raw_ns(), 1600u);

  KEXPECT_EQ(timer.forward(0_ns, at(5000)), 0u);
}

KTEST(TimerQueue, a_callback_can_start_and_cancel_timers) {
  struct Scene {
    TimerQueue* queue{nullptr};
    Timer first{};
    Timer victim{};
    Timer started{};
    std::uint32_t victim_runs{0};
    std::uint32_t started_runs{0};
  } scene;

  scene.queue = nullptr;
  TimerQueue queue;
  scene.queue = &queue;

  scene.first.set_callback(
      [](Timer&, void* context) noexcept {
        auto* s = static_cast<Scene*>(context);
        (void)s->victim.cancel();
        (void)s->queue->add(s->started, at(150));
        return TimerAction::Done;
      },
      &scene);

  scene.victim.set_callback(
      [](Timer&, void* context) noexcept {
        ++static_cast<Scene*>(context)->victim_runs;
        return TimerAction::Done;
      },
      &scene);

  scene.started.set_callback(
      [](Timer&, void* context) noexcept {
        ++static_cast<Scene*>(context)->started_runs;
        return TimerAction::Done;
      },
      &scene);

  (void)queue.add(scene.first, at(100));
  (void)queue.add(scene.victim, at(200));

  KEXPECT_EQ(queue.expire(300), 2u);
  KEXPECT_EQ(scene.victim_runs, 0u);
  KEXPECT_EQ(scene.started_runs, 1u);
}

KTEST(TimerQueue, a_callback_that_starts_itself_is_not_restarted_as_well) {
  struct Scene {
    TimerQueue* queue{nullptr};
    Timer timer{};
    std::uint32_t runs{0};
  } scene;

  TimerQueue queue;
  scene.queue = &queue;
  scene.timer.set_callback(
      [](Timer& timer, void* context) noexcept {
        auto* s = static_cast<Scene*>(context);
        if (++s->runs == 1) {
          (void)s->queue->add(timer, at(500));
        }

        // Asking for a restart on top of that would put it in the tree twice.
        return TimerAction::Restart;
      },
      &scene);

  (void)queue.add(scene.timer, at(100));

  KEXPECT_EQ(queue.expire(100), 1u);
  KEXPECT(queue.is_consistent());
  KEXPECT(scene.timer.is_pending());
  KEXPECT_EQ(scene.timer.deadline().raw_ns(), 500u);

  (void)scene.timer.cancel();
}

KTEST(TimerQueue, a_timer_stuck_in_the_past_cannot_take_over_the_cpu) {
  struct Scene {
    Timer timer{};
    std::uint32_t runs{0};
  } scene;

  scene.timer.set_callback(
      [](Timer&, void* context) noexcept {
        ++static_cast<Scene*>(context)->runs;
        return TimerAction::Restart;
      },
      &scene);

  TimerQueue queue;
  (void)queue.add(scene.timer, at(100));

  const std::uint32_t expired = queue.expire(100);

  KEXPECT_EQ(expired, scene.runs);
  KEXPECT_LE(expired, 256u);
  KEXPECT(scene.timer.is_pending());

  (void)scene.timer.cancel();
}

KTEST(TimerQueue, the_queue_survives_any_churn) {
  constexpr std::size_t COUNT = 256;
  static std::array<Timer, COUNT> timers;

  for (Timer& timer : timers) {
    timer.set_callback([](Timer&, void*) noexcept { return TimerAction::Done; });
  }

  TimerQueue queue;
  Rng rng{0x71AE};
  std::uint64_t now = 0;

  for (std::uint32_t step = 0; step < 20000; ++step) {
    Timer& timer = timers[rng.below(COUNT)];

    switch (rng.below(3)) {
      case 0:
        (void)timer.cancel();
        break;
      case 1:
        (void)queue.add(timer, at(now + rng.below(1000)));
        break;
      default:
        now += rng.below(50);
        (void)queue.expire(now);
        break;
    }

    if (step % 64 == 0) {
      KREQUIRE(queue.is_consistent());
    }
  }

  for (Timer& timer : timers) {
    (void)timer.cancel();
  }

  KEXPECT(queue.first() == nullptr);
}

KTEST(LatencyEstimator, settles_on_what_it_measures) {
  LatencyEstimator estimator;

  for (std::uint32_t i = 0; i < 200; ++i) {
    estimator.observe(4000);
  }

  // A steady latency: nothing on top, give or take what integer averages leave behind.
  KEXPECT_GE(estimator.advance_ns(), 3900u);
  KEXPECT_LE(estimator.advance_ns(), 4100u);
}

KTEST(LatencyEstimator, leaves_more_room_when_latency_varies) {
  LatencyEstimator steady;
  LatencyEstimator noisy;

  for (std::uint32_t i = 0; i < 200; ++i) {
    steady.observe(3000);
    noisy.observe(i % 2 == 0 ? 2000 : 4000);
  }

  KEXPECT_GT(noisy.advance_ns(), steady.advance_ns() + 2000);
}

KTEST(LatencyEstimator, does_not_learn_from_something_else_keeping_interrupts_off) {
  LatencyEstimator estimator;
  const std::uint64_t before = estimator.advance_ns();

  estimator.observe(LatencyEstimator::MAX_LATENCY_NS + 1);
  estimator.observe(100'000'000);

  KEXPECT_EQ(estimator.advance_ns(), before);
}

KTEST(LatencyEstimator, has_an_upper_limit) {
  LatencyEstimator estimator;

  for (std::uint32_t i = 0; i < 200; ++i) {
    estimator.observe(i % 2 == 0 ? 0 : LatencyEstimator::MAX_LATENCY_NS);
  }

  KEXPECT_LE(estimator.advance_ns(), LatencyEstimator::MAX_LATENCY_NS);
}

// ---- Real hardware
// -------------------------------------------------------------------------------

KTEST_ALL_CPUS(Timer, every_cpu_has_its_own_queue, cpu) {
  TimerQueue& queue = hw::percpu::timers();

  KEXPECT(&queue == &hw::percpu::cpu_area(cpu)->timers);

  const auto address = reinterpret_cast<std::uintptr_t>(&queue);
  const auto area = reinterpret_cast<std::uintptr_t>(hw::percpu::self());
  KEXPECT_GE(address, area);
  KEXPECT_LT(address, area + sizeof(hw::PerCpu));
}

KTEST_ALL_CPUS(Timer, never_goes_off_early_and_stays_on_its_cpu, cpu) {
  Rng rng{cpu + 1};

  for (std::uint32_t round = 0; round < 300; ++round) {
    Probe probe;
    Timer timer{&probe_fired, &probe};

    const TimePoint deadline =
        SystemClock::now() +
        Duration::nanoseconds(static_cast<std::int64_t>(1 + rng.below(200'000)));
    (void)timer.start_at(deadline);

    KREQUIRE(wait_for([&] { return probe.count.load(std::memory_order_acquire) == 1; }));
    timer.cancel_sync();

    KEXPECT_GE(probe.fired_ns.load(std::memory_order_relaxed), deadline.raw_ns());
    KEXPECT_EQ(probe.cpu.load(std::memory_order_relaxed), cpu);
  }
}

KTEST(Timer, a_deadline_in_the_past_goes_off_at_once) {
  Probe probe;
  Timer timer{&probe_fired, &probe};

  (void)timer.start_at(at(1));

  KEXPECT(wait_for([&] { return probe.count.load(std::memory_order_acquire) == 1; }, 100_ms));
  timer.cancel_sync();
}

KTEST(Timer, real_timers_run_in_order) {
  struct Scene {
    std::atomic<std::uint32_t> runs{0};
    std::atomic<std::uint32_t> out_of_order{0};
    std::atomic<std::uint64_t> last_deadline{0};
  } scene;

  std::array<Timer, 16> timers;
  const TimePoint base = SystemClock::now() + 2_ms;

  for (std::uint32_t i = 0; i < timers.size(); ++i) {
    timers[i].set_callback(
        [](Timer& timer, void* context) noexcept {
          auto* s = static_cast<Scene*>(context);
          const std::uint64_t deadline = timer.deadline().raw_ns();
          if (deadline < s->last_deadline.exchange(deadline, std::memory_order_relaxed)) {
            s->out_of_order.fetch_add(1, std::memory_order_relaxed);
          }

          s->runs.fetch_add(1, std::memory_order_release);
          return TimerAction::Done;
        },
        &scene);

    (void)timers[i].start_at(base + Duration::microseconds((i * 7) % 16 * 50));
  }

  KEXPECT(wait_for([&] { return scene.runs.load(std::memory_order_acquire) == 16; }));
  KEXPECT_EQ(scene.out_of_order.load(std::memory_order_relaxed), 0u);

  for (Timer& timer : timers) {
    timer.cancel_sync();
  }
}

KTEST(Timer, a_cancelled_timer_stays_quiet) {
  Probe probe;
  Timer timer{&probe_fired, &probe};

  (void)timer.start_after(1_ms);
  KEXPECT(timer.cancel());

  settle();
  KEXPECT_EQ(probe.count.load(std::memory_order_acquire), 0u);
}

KTEST(Timer, the_hardware_is_only_touched_when_it_has_to_be) {
  settle();

  TimerQueue& queue = hw::percpu::timers();
  auto do_nothing = [](Timer&, void*) noexcept { return TimerAction::Done; };

  Timer soon{do_nothing};
  Timer later{do_nothing};
  Timer sooner{do_nothing};

  const std::uint64_t arms = queue.stats().hardware_arms;
  const TimePoint base = SystemClock::now() + 1_s;

  (void)soon.start_at(base);
  KEXPECT_EQ(queue.stats().hardware_arms, arms + 1);

  // Not the first: nothing changes for the hardware.
  (void)later.start_at(base + 1_s);
  KEXPECT_EQ(queue.stats().hardware_arms, arms + 1);

  KEXPECT(later.cancel());
  KEXPECT_EQ(queue.stats().hardware_arms, arms + 1);

  (void)sooner.start_at(base - 500_ms);
  KEXPECT_EQ(queue.stats().hardware_arms, arms + 2);

  // Stopping the first one leaves the hardware as it is, to go off for nothing.
  KEXPECT(sooner.cancel());
  KEXPECT(soon.cancel());
  KEXPECT_EQ(queue.stats().hardware_arms, arms + 2);
}

// A long wait goes through in several legs where the hardware cannot do it in one. Seeing that for
// real takes seconds or minutes, depending on the APIC's clock, so this makes the limit shorter.
KTEST(Timer, a_long_wait_goes_in_legs_and_ends_on_time) {
  settle();

  TimerQueue& queue = hw::percpu::timers();
  queue.limit_hardware_wait(200'000);

  Probe probe;
  Timer timer{&probe_fired, &probe};

  const std::uint64_t arms = queue.stats().hardware_arms;
  const TimePoint deadline = SystemClock::now() + 2_ms;
  (void)timer.start_at(deadline);

  const bool fired = wait_for([&] { return probe.count.load(std::memory_order_acquire) == 1; });
  queue.limit_hardware_wait(hw::percpu::lapic().max_timer_delay_ns());

  KEXPECT(fired);
  KEXPECT_GE(probe.fired_ns.load(std::memory_order_relaxed), deadline.raw_ns());
  KEXPECT_GE(queue.stats().hardware_arms, arms + 8);
}

KTEST(Timer, can_be_started_on_another_cpu) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  const std::uint32_t target = smp::online_cpus() - 1;

  // That CPU is asleep, with nothing else that could wake it up.
  settle();

  Probe probe;
  Timer timer{&probe_fired, &probe};
  const TimePoint deadline = SystemClock::now() + 500_us;

  const auto started = timer.start_on(target, deadline);
  KREQUIRE(started.has_value());
  KEXPECT(!*started);

  KEXPECT(wait_for([&] { return probe.count.load(std::memory_order_acquire) == 1; }));
  KEXPECT_EQ(probe.cpu.load(std::memory_order_relaxed), target);
  KEXPECT_GE(probe.fired_ns.load(std::memory_order_relaxed), deadline.raw_ns());
  timer.cancel_sync();
}

KTEST(Timer, refuses_a_cpu_that_is_not_there) {
  Timer timer{&probe_fired, nullptr};

  const auto started = timer.start_on(smp::possible_cpus() + 1, SystemClock::now() + 1_ms);

  KREQUIRE(!started.has_value());
  KEXPECT(started.error() == Error::InvalidState);
  KEXPECT(!timer.is_pending());
}

KTEST(Timer, cancel_sync_waits_for_a_callback_on_another_cpu) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  struct Scene {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
  } scene;

  Timer timer{[](Timer&, void* context) noexcept {
                auto* s = static_cast<Scene*>(context);
                s->started.store(true, std::memory_order_release);

                const TimePoint end = SystemClock::now() + 20_ms;
                while (SystemClock::now() < end) {
                  hw::pause();
                }

                s->finished.store(true, std::memory_order_release);
                return TimerAction::Done;
              },
              &scene};

  KEXPECT(timer.start_on(smp::online_cpus() - 1, SystemClock::now() + 100_us).has_value());
  KEXPECT(wait_for([&] { return scene.started.load(std::memory_order_acquire); }));

  // Too late to stop it, which is what the two kinds of cancel are about.
  KEXPECT(!timer.cancel());
  KEXPECT(!scene.finished.load(std::memory_order_acquire));

  timer.cancel_sync();
  KEXPECT(scene.finished.load(std::memory_order_acquire));
}

KTEST(Timer, cancel_sync_does_not_wait_for_itself) {
  Probe probe;

  Timer timer{[](Timer& self, void* context) noexcept {
                self.cancel_sync();
                return probe_fired(self, context);
              },
              &probe};

  (void)timer.start_after(100_us);

  KEXPECT(wait_for([&] { return probe.count.load(std::memory_order_acquire) == 1; }));
  timer.cancel_sync();
}

KTEST_ALL_CPUS(Timer, one_timer_can_be_fought_over_by_every_cpu, cpu) {
  if (cpu == 0) {
    g_contested_runs.store(0, std::memory_order_relaxed);
    g_contested.set_callback([](Timer&, void*) noexcept {
      g_contested_runs.fetch_add(1, std::memory_order_relaxed);
      return TimerAction::Done;
    });
  }

  sync_cpus();

  Rng rng{cpu + 100};
  for (std::uint32_t round = 0; round < 20000; ++round) {
    switch (rng.below(3)) {
      case 0:
        (void)g_contested.start_after(
            Duration::nanoseconds(static_cast<std::int64_t>(rng.below(20'000))));
        break;
      case 1:
        (void)g_contested.cancel();
        break;
      default:
        (void)g_contested.start_on(static_cast<std::uint32_t>(rng.below(cpu_count())),
                                   SystemClock::now() + Duration::microseconds(5));
        break;
    }
  }

  sync_cpus();
  g_contested.cancel_sync();
  sync_cpus();

  KEXPECT(!g_contested.is_pending());
  KEXPECT(hw::percpu::timers().is_consistent());

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_GT(g_contested_runs.load(std::memory_order_relaxed), 0u);
  }
}

KTEST(Timer, a_periodic_timer_does_not_drift) {
  struct Scene {
    std::atomic<std::uint32_t> runs{0};
    std::atomic<std::uint64_t> last_deadline{0};
    std::atomic<std::uint64_t> missed{0};
  } scene;

  Timer timer{[](Timer& self, void* context) noexcept {
                auto* s = static_cast<Scene*>(context);
                s->last_deadline.store(self.deadline().raw_ns(), std::memory_order_relaxed);

                s->missed.fetch_add(self.forward(PERIOD) - 1, std::memory_order_relaxed);
                return s->runs.fetch_add(1, std::memory_order_release) + 1 < PERIODS
                           ? TimerAction::Restart
                           : TimerAction::Done;
              },
              &scene};

  const TimePoint first = SystemClock::now() + 1_ms;
  (void)timer.start_at(first);

  KEXPECT(wait_for([&] { return scene.runs.load(std::memory_order_acquire) == PERIODS; }));
  timer.cancel_sync();

  // Every period that went by is in here, whether it got a callback or was skipped.
  const std::uint64_t periods = PERIODS - 1 + scene.missed.load(std::memory_order_relaxed);
  KEXPECT_EQ(scene.last_deadline.load(std::memory_order_relaxed),
             first.raw_ns() + periods * static_cast<std::uint64_t>(PERIOD.to_ns()));
}

KTEST(Timer, can_wake_a_thread) {
  struct Scene {
    std::atomic<sched::Thread*> sleeper{nullptr};
    std::atomic<bool> time_is_up{false};
    std::atomic<bool> awake{false};
    Timer timer{};
    TimePoint deadline{};
    std::atomic<std::uint64_t> woke_at{0};
  } scene;

  scene.timer.set_callback(
      [](Timer&, void* context) noexcept {
        auto* s = static_cast<Scene*>(context);
        s->time_is_up.store(true, std::memory_order_release);
        (void)sched::Scheduler::wake(s->sleeper.load(std::memory_order_acquire));
        return TimerAction::Done;
      },
      &scene);

  const auto created = sched::Thread::spawn_kernel(
      "sleeper",
      +[](Scene* s) noexcept {
        s->sleeper.store(sched::current(), std::memory_order_release);
        s->deadline = SystemClock::now() + 1_ms;
        (void)s->timer.start_at(s->deadline);

        while (true) {
          sched::Scheduler::prepare_to_block();
          if (s->time_is_up.load(std::memory_order_acquire)) {
            sched::Scheduler::cancel_block();
            break;
          }

          sched::Scheduler::schedule();
        }

        s->woke_at.store(SystemClock::now().raw_ns(), std::memory_order_relaxed);
        s->awake.store(true, std::memory_order_release);
      },
      &scene);
  KREQUIRE(created.has_value());
  KEXPECT(sched::Scheduler::enqueue(*created).has_value());

  const TimePoint give_up = SystemClock::now() + 10_s;
  while (!scene.awake.load(std::memory_order_acquire) && SystemClock::now() < give_up) {
    sched::Scheduler::yield();
    hw::pause();
  }

  KEXPECT(scene.awake.load(std::memory_order_acquire));
  KEXPECT_GE(scene.woke_at.load(std::memory_order_relaxed), scene.deadline.raw_ns());
  scene.timer.cancel_sync();
}

// What all of this is for. The numbers go to the console, as they say more than a pass does, and
// what is asserted is what must hold anywhere: on a VM, under TCG, or on a loaded CI host.
KTEST(Timer, exact_timers_start_on_their_deadline) {
  constexpr std::size_t ROUNDS = 2000;
  static std::array<std::uint64_t, ROUNDS> normal;
  static std::array<std::uint64_t, ROUNDS> exact;

  const auto measure = [](const TimerPrecision precision, std::array<std::uint64_t, ROUNDS>& out) {
    Rng rng{0x7E57};

    for (std::size_t round = 0; round < ROUNDS; ++round) {
      Probe probe;
      Timer timer{&probe_fired, &probe, precision};

      const TimePoint deadline =
          SystemClock::now() +
          Duration::nanoseconds(static_cast<std::int64_t>(100'000 + rng.below(100'000)));
      (void)timer.start_at(deadline);

      while (probe.count.load(std::memory_order_acquire) == 0) {
        hw::pause();
      }

      timer.cancel_sync();

      const std::uint64_t fired = probe.fired_ns.load(std::memory_order_relaxed);
      out[round] = fired >= deadline.raw_ns() ? fired - deadline.raw_ns() : ~std::uint64_t{0};
    }
  };

  // The first rounds are spent finding out how long interrupts take here.
  const std::uint64_t advance_before = hw::percpu::timers().advance_ns();
  measure(TimerPrecision::Normal, normal);
  measure(TimerPrecision::Normal, normal);
  KEXPECT_NE(hw::percpu::timers().advance_ns(), advance_before);

  measure(TimerPrecision::Exact, exact);

  const std::uint64_t worst_normal = *std::ranges::max_element(normal);
  const std::uint64_t worst_exact = *std::ranges::max_element(exact);
  const std::uint64_t median_normal = median(normal);
  const std::uint64_t median_exact = median(exact);
  const std::uint64_t p99_exact = exact[ROUNDS * 99 / 100];
  const std::uint64_t p99_normal = normal[ROUNDS * 99 / 100];

  utils::logger::info("Timers: normal {} ns median, {} ns p99, {} ns worst\n", median_normal,
                      p99_normal, worst_normal);
  utils::logger::info("Timers: exact  {} ns median, {} ns p99, {} ns worst, {} ns advance\n",
                      median_exact, p99_exact, worst_exact, hw::percpu::timers().advance_ns());

  // Nothing early, in either kind, which is what the marker would have shown.
  KEXPECT_NE(worst_normal, ~std::uint64_t{0});
  KEXPECT_NE(worst_exact, ~std::uint64_t{0});

  // A typical exact timer has to beat a typical normal one by a wide margin. The worst case is out
  // of this code's hands: on a VM, the host can take the CPU away at any moment.
  KEXPECT_LE(median_exact, median_normal / 2);
}
}  // namespace kernel::testing
