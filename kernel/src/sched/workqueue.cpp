#include "sched/workqueue.hpp"

#include <algorithm>
#include <array>
#include <kformat.hpp>
#include <memory>

#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "sched/thread.hpp"
#include "utils/logger.hpp"

namespace kernel::sched {
// A worker's own state, on its stack for as long as it lives.
struct WorkerPool::Worker {
  WorkItem* current{nullptr};      ///< What it runs right now.
  WorkItem* handed_head{nullptr};  ///< Items that were found running here, to run next.
  WorkItem* handed_tail{nullptr};
  Worker* next_busy{nullptr};
};

void WorkerPool::start(const std::uint32_t node_id, const std::uint32_t max_workers) noexcept {
  m_node = node_id;
  m_max_workers = std::max(max_workers, MIN_WORKERS);

  {
    utils::IrqSaveGuard guard{m_lock};
    m_workers = 1;
  }

  spawn_worker();
}

bool WorkerPool::queue(WorkItem& item) noexcept {
  WorkerPool* expected = nullptr;
  if (!item.m_pool.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
    return false;
  }

  {
    utils::IrqSaveGuard guard{m_lock};
    item.m_next = nullptr;
    if (m_tail != nullptr) {
      m_tail->m_next = &item;
    } else {
      m_head = &item;
    }

    m_tail = &item;
    m_queued.store(m_queued.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  (void)m_idle_wq.wake_one();
  return true;
}

bool WorkerPool::cancel(WorkItem& item) noexcept {
  utils::IrqSaveGuard guard{m_lock};
  if (item.m_pool.load(std::memory_order_relaxed) != this) {
    return false;
  }

  // In the queue, or handed to whoever runs it right now. Either way the link is unique.
  const auto unlink = [&](WorkItem*& head, WorkItem*& tail) {
    WorkItem* prev = nullptr;
    for (WorkItem* it = head; it != nullptr; prev = it, it = it->m_next) {
      if (it == &item) {
        (prev != nullptr ? prev->m_next : head) = it->m_next;
        if (tail == it) {
          tail = prev;
        }

        return true;
      }
    }

    return false;
  };

  if (unlink(m_head, m_tail)) {
    m_queued.store(m_queued.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
  } else {
    for (Worker* w = m_busy; w != nullptr; w = w->next_busy) {
      if (unlink(w->handed_head, w->handed_tail)) {
        break;
      }
    }
  }

  item.m_next = nullptr;
  item.m_pool.store(nullptr, std::memory_order_release);
  return true;
}

void WorkerPool::flush(WorkItem& item) noexcept {
  m_flush_wq.wait([&] {
    utils::IrqSaveGuard guard{m_lock};
    return item.m_pool.load(std::memory_order_relaxed) != this && running_locked(item) == nullptr;
  });
}

WorkerPool::Stats WorkerPool::stats() noexcept {
  utils::IrqSaveGuard guard{m_lock};
  return {.workers = m_workers, .idle = m_idle, .completed = m_completed};
}

WorkerPool::Worker* WorkerPool::running_locked(const WorkItem& item) const noexcept {
  for (Worker* w = m_busy; w != nullptr; w = w->next_busy) {
    if (w->current == &item) {
      return w;
    }
  }

  return nullptr;
}

WorkItem* WorkerPool::take_locked(Worker& self) noexcept {
  while (true) {
    WorkItem* item = self.handed_head;
    if (item != nullptr) {
      self.handed_head = item->m_next;
      if (self.handed_head == nullptr) {
        self.handed_tail = nullptr;
      }
    } else {
      item = m_head;
      if (item == nullptr) {
        return nullptr;
      }

      m_head = item->m_next;
      if (m_head == nullptr) {
        m_tail = nullptr;
      }

      m_queued.store(m_queued.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);

      // Running somewhere already: that worker runs it again once it is done, and not this one at
      // the same time.
      if (Worker* owner = running_locked(*item); owner != nullptr && owner != &self) {
        item->m_next = nullptr;
        if (owner->handed_tail != nullptr) {
          owner->handed_tail->m_next = item;
        } else {
          owner->handed_head = item;
        }

        owner->handed_tail = item;
        continue;
      }
    }

    // From here on it is running, not waiting, and can be queued again.
    item->m_next = nullptr;
    item->m_pool.store(nullptr, std::memory_order_release);
    return item;
  }
}

bool WorkerPool::wait_for_work() noexcept {
  bool extra;
  {
    utils::IrqSaveGuard guard{m_lock};
    ++m_idle;
    extra = m_workers > MIN_WORKERS;
  }

  const auto work_waits = [this] { return m_queued.load(std::memory_order_acquire) != 0; };

  // The first worker never times out, so an idle pool asks nothing of the timers.
  bool found = true;
  if (extra) {
    found = m_idle_wq.wait_for(RETIRE_AFTER, work_waits);
  } else {
    m_idle_wq.wait(work_waits);
  }

  utils::IrqSaveGuard guard{m_lock};
  --m_idle;

  if (!found && m_head == nullptr && m_workers > MIN_WORKERS) {
    --m_workers;
    return false;
  }

  return true;
}

void WorkerPool::spawn_worker() noexcept {
  std::uint32_t number;
  {
    utils::IrqSaveGuard guard{m_lock};
    number = m_next_name++;
  }

  std::array<char, Thread::MAX_NAME_LEN> name{};
  (void)klib::kprint(name.data(), name.size(), "kworker/{}:{}", m_node, number);

  const ThreadCreateOptions options{.preferred_numa_node = m_node};
  const auto thread = Thread::spawn_kernel(name.data(), &worker_main, this, options);
  if (thread && Scheduler::enqueue(*thread)) {
    return;
  }

  // Counted before it was made, so that two workers do not start one each for the same reason.
  utils::IrqSaveGuard guard{m_lock};
  --m_workers;
  utils::logger::warn("workqueue: cannot start a worker for node {}\n", m_node);
}

void WorkerPool::worker_main(WorkerPool* pool) noexcept { pool->run_worker(); }

void WorkerPool::run_worker() noexcept {
  Worker self{};

  while (true) {
    WorkItem* item;
    bool needs_company = false;
    {
      utils::IrqSaveGuard guard{m_lock};
      item = take_locked(self);

      if (item != nullptr) {
        self.current = item;
        self.next_busy = m_busy;
        m_busy = &self;

        // If this one blocks, the next item needs somebody else to take it.
        needs_company = m_idle == 0 && !m_spawning && m_workers < m_max_workers;
        if (needs_company) {
          m_spawning = true;
          ++m_workers;
        }
      }
    }

    if (item == nullptr) {
      if (!wait_for_work()) {
        return;
      }

      continue;
    }

    if (needs_company) {
      spawn_worker();
      utils::IrqSaveGuard guard{m_lock};
      m_spawning = false;
    }

    // The item may be freed by its own callback: nothing below looks at it again.
    item->m_callback(item->m_context);

    {
      utils::IrqSaveGuard guard{m_lock};
      for (Worker** link = &m_busy; *link != nullptr; link = &(*link)->next_busy) {
        if (*link == &self) {
          *link = self.next_busy;
          break;
        }
      }

      self.current = nullptr;
      self.next_busy = nullptr;
      ++m_completed;
    }

    if (m_flush_wq.waiters() != 0) {
      (void)m_flush_wq.wake_all();
    }
  }
}

namespace workqueue {
namespace {
WorkerPool* g_pools = nullptr;
std::uint32_t g_pool_count = 0;
}  // namespace

void initialize() noexcept {
  const std::uint32_t nodes = hw::smp::node_count();
  auto* pools = static_cast<WorkerPool*>(
      memory::heap::kmalloc(sizeof(WorkerPool) * nodes, alignof(WorkerPool)));
  if (pools == nullptr) {
    utils::logger::fatal("workqueue: no memory for {} pools\n", nodes);
  }

  for (std::uint32_t node = 0; node < nodes; ++node) {
    std::construct_at(&pools[node]);
  }

  g_pools = pools;
  g_pool_count = nodes;

  for (std::uint32_t node = 0; node < nodes; ++node) {
    std::uint32_t node_cpus = 0;
    hw::smp::for_each_online([&](const std::uint32_t cpu) {
      node_cpus += hw::smp::entry(cpu).numa_node == node ? 1 : 0;
    });

    // Enough for every CPU of the node to have one blocked and one running, within reason.
    pools[node].start(node, std::clamp(2 * node_cpus, 4u, 64u));
  }
}

WorkerPool& pool(const std::uint32_t node_id) noexcept {
  KASSERT(node_id < g_pool_count);
  return g_pools[node_id];
}

bool queue(WorkItem& item) noexcept { return queue_on(hw::percpu::numa_node_id(), item); }

bool queue_on(const std::uint32_t node_id, WorkItem& item) noexcept {
  if (g_pools == nullptr || node_id >= g_pool_count) [[unlikely]] {
    return false;
  }

  return g_pools[node_id].queue(item);
}
}  // namespace workqueue
}  // namespace kernel::sched
