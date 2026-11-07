// Work items: where they run, how often, and that they cannot get in each other's way.
#include "sched/workqueue.hpp"

#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "hal/timer/timer.hpp"
#include "ktest.hpp"
#include "memory/heap.hpp"
#include "sched/completion.hpp"
#include "sched/mutex.hpp"
#include "sched/sleep.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using sched::WorkerPool;
using sched::WorkItem;
using support::wait_for;
namespace workqueue = sched::workqueue;

[[nodiscard]] WorkerPool& local_pool() noexcept {
  return workqueue::pool(hw::percpu::numa_node_id());
}

struct Context {
  std::atomic<std::uint32_t> runs{0};
  std::atomic<bool> could_sleep{false};
  std::atomic<bool> in_a_worker{false};
  std::atomic<bool> slept{false};
};

void look_around(void* context) noexcept {
  auto* seen = static_cast<Context*>(context);
  seen->could_sleep.store(hw::percpu::can_sleep(), std::memory_order_relaxed);
  seen->in_a_worker.store(sched::current()->name().starts_with("kworker/"),
                          std::memory_order_relaxed);

  // The point of a worker: it can wait, and take a Mutex.
  sched::Mutex mutex;
  mutex.lock();
  seen->slept.store(sched::sleep_for(Duration::milliseconds(1)), std::memory_order_relaxed);
  mutex.unlock();

  seen->runs.fetch_add(1, std::memory_order_acq_rel);
}

void count_run(void* context) noexcept {
  static_cast<Context*>(context)->runs.fetch_add(1, std::memory_order_acq_rel);
}
}  // namespace

KTEST(Workqueue, runs_an_item_in_a_worker_that_can_sleep) {
  Context seen;
  WorkItem item{&look_around, &seen};

  KEXPECT(workqueue::queue(item));
  KEXPECT(wait_for([&] { return seen.runs.load(std::memory_order_acquire) == 1; }));
  local_pool().flush(item);

  KEXPECT(seen.could_sleep.load());
  KEXPECT(seen.in_a_worker.load());
  KEXPECT(seen.slept.load());
  KEXPECT(!item.is_pending());
}

// Queued twice before it got to run: once is enough, and queue() says so.
KTEST(Workqueue, an_item_waiting_already_is_not_queued_again) {
  struct Scene {
    sched::Completion release;
    Context counted;
  } scene;

  // Keeps a worker busy until the second queue() has been tried.
  WorkItem blocker{[](void* s) noexcept { static_cast<Scene*>(s)->release.wait(); }, &scene};
  WorkItem item{&count_run, &scene.counted};

  // Every worker of the pool could be idle, so the item may start before anything else is tried.
  // What has to hold is that it does not run more often than queue() said yes.
  KEXPECT(workqueue::queue(blocker));
  const bool first = workqueue::queue(item);
  const bool second = workqueue::queue(item);
  KEXPECT(first);

  scene.release.complete_all();
  local_pool().flush(blocker);
  local_pool().flush(item);

  KEXPECT_EQ(scene.counted.runs.load(), second ? 2u : 1u);
}

// An item that queues itself again while it runs runs again, but never next to itself, however
// many idle workers there are to take it.
KTEST(Workqueue, an_item_never_runs_twice_at_once) {
  struct Scene {
    std::atomic<std::uint32_t> inside{0};
    std::atomic<std::uint32_t> most_inside{0};
    std::atomic<std::uint32_t> runs{0};
    std::atomic<std::uint32_t> queued{1};
    WorkItem* self{nullptr};
  } scene;

  constexpr std::uint32_t RUNS = 50;

  WorkItem item{[](void* s) noexcept {
                  auto* scene = static_cast<Scene*>(s);
                  const std::uint32_t now = scene->inside.fetch_add(1) + 1;
                  std::uint32_t most = scene->most_inside.load();
                  while (most < now && !scene->most_inside.compare_exchange_weak(most, now)) {
                  }

                  // Queued again while it runs, more than once: those have to wait for this run.
                  for (int i = 0; i < 3 && scene->queued.load() < RUNS; ++i) {
                    if (sched::workqueue::queue(*scene->self)) {
                      scene->queued.fetch_add(1);
                    }
                  }

                  support::spin_for(Duration::microseconds(50));
                  scene->inside.fetch_sub(1);
                  scene->runs.fetch_add(1, std::memory_order_acq_rel);
                },
                &scene};
  scene.self = &item;

  KEXPECT(workqueue::queue(item));
  KEXPECT(wait_for([&] {
    return scene.runs.load(std::memory_order_acquire) == scene.queued.load() && !item.is_pending();
  }));
  local_pool().flush(item);

  KEXPECT_EQ(scene.runs.load(), scene.queued.load());
  KEXPECT_GE(scene.runs.load(), RUNS);
  KEXPECT_EQ(scene.most_inside.load(), 1u);
}

// The first item waits for one queued after it. That only ends if the pool has someone else to
// take the second while the first worker is stuck.
KTEST(Workqueue, a_blocked_item_does_not_hold_up_the_next) {
  struct Scene {
    sched::Completion second_ran;
    std::atomic<bool> first_done{false};
  } scene;

  WorkItem first{[](void* s) noexcept {
                   auto* scene = static_cast<Scene*>(s);
                   scene->first_done.store(scene->second_ran.wait_for(Duration::seconds(10)),
                                           std::memory_order_release);
                 },
                 &scene};
  WorkItem second{[](void* s) noexcept { static_cast<Scene*>(s)->second_ran.complete_all(); },
                  &scene};

  KEXPECT(workqueue::queue(first));
  KEXPECT(workqueue::queue(second));

  local_pool().flush(first);
  local_pool().flush(second);
  KEXPECT(scene.first_done.load(std::memory_order_acquire));
}

// From interrupt context, where queueing has to work for the whole thing to make sense.
KTEST(Workqueue, an_interrupt_handler_can_queue_work) {
  struct Scene {
    Context counted;
    WorkItem item{&count_run, &counted};
    std::atomic<bool> queued{false};
  } scene;

  hw::timer::Timer timer{[](hw::timer::Timer&, void* s) noexcept {
                           auto* scene = static_cast<Scene*>(s);
                           scene->queued.store(sched::workqueue::queue(scene->item),
                                               std::memory_order_release);
                           return hw::timer::TimerAction::Done;
                         },
                         &scene};

  // Nothing in between may leave early: the timer and the scene are on this stack.
  KEXPECT(!timer.start_after(Duration::milliseconds(1)));
  KEXPECT(wait_for([&] { return scene.counted.runs.load(std::memory_order_acquire) == 1; }));
  timer.cancel_sync();
  local_pool().flush(scene.item);
  KEXPECT(scene.queued.load(std::memory_order_acquire));
}

KTEST(Workqueue, flush_waits_for_a_run_to_end) {
  struct Scene {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
  } scene;

  WorkItem item{[](void* s) noexcept {
                  auto* scene = static_cast<Scene*>(s);
                  scene->started.store(true, std::memory_order_release);
                  (void)sched::sleep_for(Duration::milliseconds(20));
                  scene->finished.store(true, std::memory_order_release);
                },
                &scene};

  KEXPECT(workqueue::queue(item));
  KEXPECT(wait_for([&] { return scene.started.load(std::memory_order_acquire); }));
  local_pool().flush(item);
  KEXPECT(scene.finished.load(std::memory_order_acquire));
}

// Whatever cancel() says has to be the truth: a cancelled item never runs.
KTEST(Workqueue, a_cancelled_item_does_not_run) {
  constexpr std::uint32_t TRIES = 200;
  Context counted;
  std::uint32_t cancelled = 0;

  for (std::uint32_t i = 0; i < TRIES; ++i) {
    WorkItem item{&count_run, &counted};
    KEXPECT(workqueue::queue(item));
    cancelled += local_pool().cancel(item);
    local_pool().flush(item);
    KEXPECT(!item.is_pending());
  }

  KEXPECT_EQ(counted.runs.load() + cancelled, TRIES);

  WorkItem never_queued{&count_run, &counted};
  KEXPECT(!local_pool().cancel(never_queued));
}

// The callback may be the end of its item: nothing touches the item once the callback started.
KTEST(Workqueue, an_item_may_free_itself) {
  struct Owned {
    WorkItem item;
    std::atomic<std::uint32_t>* runs;
  };

  std::atomic<std::uint32_t> runs{0};
  constexpr std::uint32_t ITEMS = 64;

  for (std::uint32_t i = 0; i < ITEMS; ++i) {
    void* memory = memory::heap::kmalloc(sizeof(Owned));
    KREQUIRE(memory != nullptr);

    auto* owned = new (memory) Owned{
        .item = WorkItem{[](void* self) noexcept {
                           auto* owned = static_cast<Owned*>(self);
                           owned->runs->fetch_add(1, std::memory_order_acq_rel);
                           owned->~Owned();
                           memory::heap::kfree(owned);
                         },
                         memory},
        .runs = &runs,
    };
    KEXPECT(workqueue::queue(owned->item));
  }

  KEXPECT(wait_for([&] { return runs.load(std::memory_order_acquire) == ITEMS; }));
}

KTEST(Workqueue, every_node_has_a_pool_that_works) {
  for (std::uint32_t node = 0; node < hw::smp::node_count(); ++node) {
    Context counted;
    WorkItem item{&count_run, &counted};

    KEXPECT(workqueue::queue_on(node, item));
    workqueue::pool(node).flush(item);
    KEXPECT_EQ(counted.runs.load(), 1u);
    KEXPECT_GE(workqueue::pool(node).stats().workers, 1u);
    KEXPECT_EQ(workqueue::pool(node).node(), node);
  }

  WorkItem nowhere{&count_run, nullptr};
  KEXPECT(!workqueue::queue_on(hw::smp::node_count(), nowhere));
}
}  // namespace kernel::testing
