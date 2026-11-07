/// @file workqueue.hpp
/// @brief Work for kernel threads: deferred calls that are allowed to sleep.
///
/// @code
/// void write_back(void* buffer) noexcept { ... takes a Mutex, allocates, waits for the disk ... }
///
/// constinit sched::WorkItem g_write_back{&write_back, &g_buffer};
///
/// // From anywhere, an interrupt handler included:
/// (void)sched::workqueue::queue(g_write_back);
/// @endcode
///
/// A Dpc (dpc.hpp) is quick, but runs in interrupt context: no sleeping, no Mutex, no allocating
/// anything that might wait for memory. A WorkItem runs in a `kworker` thread instead, where all of
/// that is fine, at the price of a wakeup and a context switch.
///
/// ### Pools
///
/// Every NUMA node has a pool of workers, kept near the node, and an item runs in the pool it was
/// queued on, by default the one of the CPU that queued it. A pool starts with one worker and grows
/// as items block: a worker that takes an item makes sure someone else is left to take the next,
/// starting another worker if nobody is. So an item that waits for one queued after it still gets
/// its turn. Workers that have had nothing to do for a while go away again, all but the first.
///
/// ### Guarantees
///
/// An item queued while it is waiting is not queued twice: queue() says false, and the one run
/// covers both. An item queued while it is running runs again afterwards. It never runs twice at
/// the same time in one pool: a worker that finds it running elsewhere hands it to the worker
/// that runs it, which takes it next. The callback may free the item, as long as nothing queues
/// it again; nothing here touches an item after its callback has started.
#pragma once

#include <atomic>
#include <cstdint>

#include "sched/wait_queue.hpp"
#include "utils/lock.hpp"

namespace kernel::sched {
class WorkerPool;

/// @brief A call to make in a kernel thread. See the file comment.
class WorkItem {
 public:
  using Callback = void (*)(void* context) noexcept;

  constexpr explicit WorkItem(const Callback callback, void* context = nullptr) noexcept
      : m_callback(callback), m_context(context) {}

  WorkItem(const WorkItem&) = delete;
  WorkItem& operator=(const WorkItem&) = delete;

  /// @brief Whether it is waiting to run, which it stops doing as it starts.
  [[nodiscard]] bool is_pending() const noexcept {
    return m_pool.load(std::memory_order_acquire) != nullptr;
  }

 private:
  friend class WorkerPool;

  Callback m_callback;
  void* m_context;
  WorkItem* m_next{nullptr};                 ///< In a pool's queue or a worker's hand-over list.
  std::atomic<WorkerPool*> m_pool{nullptr};  ///< Where it is waiting; null when it is not.
};

/// @brief The workers of one NUMA node. See the file comment.
class alignas(std::hardware_destructive_interference_size) WorkerPool {
 public:
  struct Stats {
    std::uint32_t workers;    ///< Running, or about to.
    std::uint32_t idle;       ///< Of those, waiting for work.
    std::uint64_t completed;  ///< Callbacks that returned, so far.
  };

  constexpr WorkerPool() noexcept = default;

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  /// @brief Sets the pool up for @p node_id and starts its first worker.
  void start(std::uint32_t node_id, std::uint32_t max_workers) noexcept;

  /// @brief Queues @p item at the end. Safe from interrupt handlers and with locks held, but not
  ///        from an NMI.
  /// @return false if it was waiting already, here or in another pool.
  bool queue(WorkItem& item) noexcept;

  /// @brief Takes @p item back if it has not started.
  /// @return Whether it was waiting here. A run that has started goes on all the same.
  bool cancel(WorkItem& item) noexcept;

  /// @brief Waits until @p item is neither waiting here nor running. It can go away then, as long
  ///        as nothing queues it again.
  /// @pre The calling thread can sleep, and is not running @p item.
  void flush(WorkItem& item) noexcept;

  [[nodiscard]] Stats stats() noexcept;

  [[nodiscard]] std::uint32_t node() const noexcept { return m_node; }

 private:
  struct Worker;

  // How long a worker beyond the first waits for work before it goes away. Long, as its wait is a
  // timer that idle CPUs would otherwise not have.
  static constexpr klib::Duration RETIRE_AFTER = klib::Duration::seconds(30);
  static constexpr std::uint32_t MIN_WORKERS = 1;

  static void worker_main(WorkerPool* pool) noexcept;
  void run_worker() noexcept;
  [[nodiscard]] WorkItem* take_locked(Worker& self) noexcept;
  [[nodiscard]] Worker* running_locked(const WorkItem& item) const noexcept;
  [[nodiscard]] bool wait_for_work() noexcept;
  void spawn_worker() noexcept;

  utils::QSpinlock m_lock{};
  WorkItem* m_head{nullptr};
  WorkItem* m_tail{nullptr};
  Worker* m_busy{nullptr};  ///< Workers with an item, for running_locked().

  std::atomic<std::uint32_t> m_queued{0};  ///< Items in the queue, for idle workers to look at.
  std::uint32_t m_workers{0};
  std::uint32_t m_idle{0};
  std::uint32_t m_max_workers{0};
  std::uint32_t m_next_name{0};
  std::uint32_t m_node{0};
  bool m_spawning{false};
  std::uint64_t m_completed{0};

  WaitQueue m_idle_wq{};   ///< Workers waiting for work.
  WaitQueue m_flush_wq{};  ///< flush() waiting for an item to be done.
};

namespace workqueue {
/// @brief Starts a pool on every NUMA node.
/// @pre The scheduler runs on every CPU. Called once.
void initialize() noexcept;

/// @brief The pool of node @p node_id.
/// @pre initialize() is done, and the node exists.
[[nodiscard]] WorkerPool& pool(std::uint32_t node_id) noexcept;

/// @brief Queues @p item in the pool of the calling CPU's node. See WorkerPool::queue().
bool queue(WorkItem& item) noexcept;

/// @brief Queues @p item in the pool of node @p node_id.
bool queue_on(std::uint32_t node_id, WorkItem& item) noexcept;
}  // namespace workqueue
}  // namespace kernel::sched
