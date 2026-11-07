#include "sched/reaper.hpp"

#include <atomic>

#include "sched/scheduler.hpp"
#include "sched/wait_queue.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::sched::reaper {
namespace {
constinit utils::QSpinlock g_lock{};
constinit utils::SList<Thread, ReapQueueTag> g_zombies{};
constinit WaitQueue g_reap_wq{};
constinit std::atomic<std::uint32_t> g_pending{0};
constinit std::atomic<std::uint64_t> g_reaped{0};

void reaper_loop() noexcept {
  while (true) {
    g_reap_wq.wait([] { return g_pending.load(std::memory_order_acquire) != 0; });

    utils::SList<Thread, ReapQueueTag> dead;
    {
      utils::IrqSaveGuard guard{g_lock};
      dead = std::move(g_zombies);
    }

    std::uint32_t count = 0;
    while (!dead.empty()) {
      Thread* thread = dead.pop_front();
      const ThreadId tid = thread->tid();

      if (const auto destroyed = Thread::destroy(thread); !destroyed) [[unlikely]] {
        utils::logger::warn("kreaperd: cannot destroy thread {}: {:#x}\n", tid,
                            std::to_underlying(destroyed.error()));
      }

      ++count;
    }

    g_reaped.fetch_add(count, std::memory_order_relaxed);
    g_pending.fetch_sub(count, std::memory_order_release);
  }
}
}  // namespace

void initialize() noexcept {
  const auto thread = Thread::spawn_kernel(
      "kreaperd", [] noexcept { reaper_loop(); },
      ThreadCreateOptions{.priority = PriorityClass::Background});
  if (!thread || !Scheduler::enqueue(*thread)) {
    utils::logger::fatal("sched: cannot start kreaperd\n");
  }
}

void enqueue_dead(utils::SList<Thread, ReapQueueTag>& dead) noexcept {
  if (dead.empty()) {
    return;
  }

  std::uint32_t count = 0;
  bool was_idle;
  {
    utils::IrqSaveGuard guard{g_lock};
    while (!dead.empty()) {
      g_zombies.push_front(*dead.pop_front());
      ++count;
    }

    was_idle = g_pending.fetch_add(count, std::memory_order_release) == 0;
  }

  // One wake for a whole batch: the reaper takes everything that is there when it looks.
  if (was_idle) {
    (void)g_reap_wq.wake_one();
  }
}

void enqueue_dead(Thread* thread) noexcept {
  utils::SList<Thread, ReapQueueTag> one;
  one.push_front(*thread);
  enqueue_dead(one);
}

std::uint64_t reaped_count() noexcept { return g_reaped.load(std::memory_order_relaxed); }

std::uint32_t pending_count() noexcept { return g_pending.load(std::memory_order_relaxed); }
}  // namespace kernel::sched::reaper
