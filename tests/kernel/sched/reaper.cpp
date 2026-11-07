// kreaperd: finished threads go away by themselves, and only once nothing is on their stacks.
#include "sched/reaper.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "sched/scheduler.hpp"
#include "sched/thread.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using sched::RunQueue;
using sched::Scheduler;
using sched::Thread;
using support::wait_for;

struct Finished {
  std::atomic<std::uint32_t> count{0};
};

void finish_at_once(Finished* finished) noexcept {
  finished->count.fetch_add(1, std::memory_order_acq_rel);
}

// What the destroy hook of a thread saw of the world it ran in.
struct Witness {
  std::atomic<bool> called{false};
  std::atomic<bool> by_the_reaper{false};
  std::atomic<bool> could_sleep{false};
  std::atomic<bool> was_dead{false};
  std::atomic<bool> was_off_its_cpu{false};
};

void witness_destroy(Thread* thread, void* context) noexcept {
  auto* seen = static_cast<Witness*>(context);
  const RunQueue& rq = Scheduler::cpu_runqueue(thread->cpu_id());

  seen->by_the_reaper.store(sched::current()->name() == "kreaperd", std::memory_order_relaxed);
  seen->could_sleep.store(hw::percpu::can_sleep(), std::memory_order_relaxed);
  seen->was_dead.store(thread->state() == sched::ThreadState::Dead, std::memory_order_relaxed);
  seen->was_off_its_cpu.store(rq.current() != thread, std::memory_order_relaxed);
  seen->called.store(true, std::memory_order_release);
}
}  // namespace

KTEST(Reaper, finished_threads_go_away_by_themselves) {
  constexpr std::uint32_t THREADS = 16;

  const std::uint64_t before = sched::reaper::reaped_count();
  Finished finished;

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(support::start("short", &finish_at_once, &finished) != nullptr);
  }

  KEXPECT(wait_for([&] { return finished.count.load(std::memory_order_acquire) == THREADS; }));
  KEXPECT(wait_for([&] { return sched::reaper::reaped_count() >= before + THREADS; }));
}

KTEST(Reaper, destroys_a_thread_off_its_cpu_in_a_thread_that_can_sleep) {
  Finished finished;
  Witness seen;

  const auto created = Thread::spawn_kernel("witnessed", &finish_at_once, &finished,
                                            support::pinned_to(support::last_cpu()));
  KREQUIRE(created.has_value());
  (*created)->set_destroy_hook(&witness_destroy, &seen);
  KREQUIRE(Scheduler::enqueue(*created).has_value());

  KEXPECT(wait_for([&] { return seen.called.load(std::memory_order_acquire); }));
  KEXPECT(seen.by_the_reaper.load());
  KEXPECT(seen.could_sleep.load());
  KEXPECT(seen.was_dead.load());
  KEXPECT(seen.was_off_its_cpu.load());
}

// Threads that end on every CPU at once all get handed over, in batches, and the stacks they leave
// behind get used again.
KTEST(Reaper, keeps_up_with_threads_ending_everywhere) {
  constexpr std::uint32_t ROUNDS = 8;
  const std::uint32_t cpus = hw::smp::online_cpus();

  const std::uint64_t reaped = sched::reaper::reaped_count();
  const std::uint64_t reused = Thread::stack_cache_stats().reused;
  Finished finished;
  std::uint32_t started = 0;

  for (std::uint32_t round = 0; round < ROUNDS; ++round) {
    for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
      started +=
          support::start("churn", &finish_at_once, &finished, support::pinned_to(cpu)) != nullptr;
    }

    // Some of them end before the next round starts, so that the next ones find stacks to take.
    KEXPECT(wait_for([&] { return finished.count.load(std::memory_order_acquire) == started; }));
    KEXPECT(wait_for([&] { return sched::reaper::pending_count() == 0; }));
  }

  KEXPECT_EQ(started, ROUNDS * cpus);
  KEXPECT(wait_for([&] { return sched::reaper::reaped_count() >= reaped + started; }));
  KEXPECT_GT(Thread::stack_cache_stats().reused, reused);
  KEXPECT_LE(Thread::stack_cache_stats().cached, Thread::STACK_CACHE_SIZE);
}
}  // namespace kernel::testing
