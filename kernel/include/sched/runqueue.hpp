/// @file runqueue.hpp
/// @brief One CPU's threads that want to run.
///
/// Every CPU has a RunQueue in its hw::PerCpu, and threads wait here in two tiers:
///
///  1. Real-time threads, in one FIFO per level. They beat everything below them and run until
///     they block, yield to their equals or lose the CPU to a higher level. Nothing shares time
///     among them on its own, so one that spins starves the CPU, which is what it asked for.
///  2. Everything else, in an EevdfQueue.
///
/// The idle thread is on neither. It is what runs when both are empty.
///
/// Most of the class is `_locked`: the caller takes the lock, usually for several of these in a
/// row. A RunQueue is a BasicLockable, so the usual guards work on it, and interrupts must be off
/// while holding it, as the scheduler's interrupt handlers take it too.
///
/// ### The lock covers a context switch
///
/// When a CPU switches threads, it takes the lock on the old thread and releases it on the new one
/// (see Scheduler::finish_switch()). So whoever holds a queue's lock sees it with nothing half
/// done: a thread that is on the queue has left its CPU for good, and the current thread is really
/// the one running. Without this, another CPU could pick up a thread that is still saving its
/// registers on the stack it is about to be started on, and telling that apart takes a good deal
/// more machinery than this does.
///
/// Nothing ever holds the locks of two queues at once, so there is no order to get wrong: moving a
/// thread means taking it off one queue, dropping that lock, and then putting it on the other.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "hal/timer/timer.hpp"
#include "sched/eevdf.hpp"
#include "utils/linked_list.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::sched {
class Thread;
class NumaDomain;

struct RunQueueTag;
struct ReapQueueTag;

/// @brief Why the running thread is looking for a successor.
enum class PickMode : std::uint8_t {
  Voluntary,  ///< It is blocking or leaving, or just wants the decision taken again.
  Yield,      ///< It would like the others to go first, but stays runnable.
  Preempted,  ///< An interrupt caught it. It stays runnable, whatever state it was preparing for.
};

/// @brief The run queue of one CPU. See the file comment.
class alignas(std::hardware_destructive_interference_size) RunQueue {
  // TestRunner, then RealTime.
  static constexpr std::uint32_t RT_LEVELS = 2;

  // How many threads a thief looks at. Pinned ones cannot be taken, and a queue full of them
  // should not hold the victim's lock for long.
  static constexpr std::uint32_t STEAL_SCAN_LIMIT = 8;

  struct RtFifo {
    utils::SList<Thread, RunQueueTag> list{};
    Thread* tail{nullptr};
  };

  // Read by other CPUs all the time, and written on every change of the queue. The idle loop
  // monitors the first of these, which is why nothing else that changes often shares its line.
  std::atomic<std::uint32_t> m_waiting{0};    ///< Threads queued, not counting the running one.
  std::atomic<std::uint32_t> m_stealable{0};  ///< The queued ones that are not pinned.
  std::atomic<Thread*> m_current{nullptr};
  std::atomic<bool> m_running_realtime{false};  ///< Says so without touching a thread from afar.
  Thread* m_idle_thread{nullptr};
  NumaDomain* m_domain{nullptr};
  std::uint32_t m_cpu_id{0};
  std::uint32_t m_slot{0};

  alignas(std::hardware_destructive_interference_size) utils::QSpinlock m_lock{};
  bool m_lock_handed_over{false};  ///< The next thread to run has to release m_lock.

  std::uint64_t m_charged_until_ns{0};  ///< The running thread has paid for its time until then.
  std::uint64_t m_switches{0};
  std::uint64_t m_rng_state{1};

  hw::timer::Timer m_slice_timer{};
  EevdfQueue m_fair{};
  std::array<RtFifo, RT_LEVELS> m_realtime{};
  utils::SList<Thread, ReapQueueTag> m_dead{};

  [[nodiscard]] std::uint32_t highest_rt_level() const noexcept;

  void account_added(const Thread& thread) noexcept;
  void account_removed(const Thread& thread) noexcept;

  void push_realtime(Thread& thread, bool at_front) noexcept;
  [[nodiscard]] Thread* pop_realtime(std::uint32_t level) noexcept;
  [[nodiscard]] bool remove_realtime(Thread& thread) noexcept;
  void requeue_current_fair() noexcept;
  void requeue_realtime(Thread& thread, bool at_front) noexcept;

  [[nodiscard]] Thread* find_stealable() noexcept;

  [[nodiscard]] Thread* switch_in(Thread& next, const Thread& prev) noexcept;

 public:
  /// @brief Real-time levels from the top, for a thread that has one.
  /// @return RT_LEVELS or above for a thread that is not real-time.
  [[nodiscard]] static std::uint32_t rt_level(const Thread& thread) noexcept;

  constexpr RunQueue() noexcept = default;

  RunQueue(const RunQueue&) = delete;
  RunQueue& operator=(const RunQueue&) = delete;

  /// @brief Gets the queue ready for use.
  /// @param cpu_id      The CPU the queue belongs to.
  /// @param domain      The CPU's domain, or null for a queue that is not connected to one.
  /// @param slot        The CPU's slot in @p domain.
  /// @param idle_thread What runs when nothing else does, and is running now.
  void initialize(std::uint32_t cpu_id, NumaDomain* domain, std::uint32_t slot,
                  Thread* idle_thread) noexcept;

  void lock() noexcept { m_lock.lock(); }
  void unlock() noexcept { m_lock.unlock(); }

  /// @brief Leaves the lock for the next thread to run to release, across a context switch.
  void hand_over_lock() noexcept { m_lock_handed_over = true; }

  /// @brief Whether the lock was left to the caller, which then has to release it.
  [[nodiscard]] bool take_lock_handover() noexcept {
    const bool handed_over = m_lock_handed_over;
    m_lock_handed_over = false;
    return handed_over;
  }

  /// @brief Charges the running thread for its time up to @p now_ns.
  void update_clock_locked(std::uint64_t now_ns) noexcept;

  /// @brief Puts @p thread on the queue.
  /// @return Whether it should take the CPU from what is running now.
  /// @pre The thread is not on any queue, and has left whatever CPU it was on.
  bool enqueue_locked(Thread& thread) noexcept;

  /// @brief Takes a waiting @p thread off the queue.
  /// @return false if it is not on this queue.
  bool remove_locked(Thread& thread) noexcept;

  /// @brief Decides what runs next, and files @p prev accordingly: back on the queue if it can
  ///        still run, nowhere if it blocked, and on the dead list if it is `Dead`.
  /// @param prev What is running now. It is what gets returned if it should carry on.
  /// @param mode Why the question came up.
  /// @return The thread to run, which is @p prev or the idle thread if nothing else is there.
  [[nodiscard]] Thread* pick_next_locked(Thread& prev,
                                         PickMode mode = PickMode::Voluntary) noexcept;

  /// @brief Whether pick_next_locked() would choose something other than what is running.
  [[nodiscard]] bool should_switch_locked() const noexcept;

  /// @brief Takes a thread off this queue for another CPU, if the lock is free and spare() is at
  ///        least @p enough. Never waits: there are always other queues to try.
  /// @pre Interrupts are off.
  [[nodiscard]] Thread* try_steal(std::uint32_t enough) noexcept;

  /// @brief Destroys the threads on the dead list. Their stacks are gone, so this cannot run on
  ///        one of them, and it takes a thread context that can wait for other CPUs.
  /// @return How many there were.
  std::size_t drain_dead() noexcept;

  /// @brief Whether a slice timer has something to do: there is someone to take turns with.
  [[nodiscard]] bool wants_timer_locked() const noexcept;

  /// @brief How long the running thread has until its slice is up.
  [[nodiscard]] std::uint64_t slice_left_ns_locked() const noexcept {
    return m_fair.slice_left_ns();
  }

  /// @brief What keeps threads taking turns on this CPU. Only its own CPU starts it.
  [[nodiscard]] hw::timer::Timer& slice_timer() noexcept { return m_slice_timer; }

  /// @brief Whether initialize() has run.
  [[nodiscard]] bool is_ready() const noexcept { return m_idle_thread != nullptr; }

  /// @brief What is running, as far as the queue knows.
  [[nodiscard]] Thread* current() const noexcept {
    return m_current.load(std::memory_order_acquire);
  }

  [[nodiscard]] Thread* idle_thread() const noexcept { return m_idle_thread; }

  /// @brief Threads waiting to run.
  [[nodiscard]] std::uint32_t waiting() const noexcept {
    return m_waiting.load(std::memory_order_acquire);
  }

  /// @brief How much another CPU would help: the threads it could take, plus one if they are stuck.
  ///
  /// A thread waiting behind an ordinary one gets its turn soon, so it is not worth much to move
  /// it far from its memory. A real-time thread does not take turns, so one behind that has no
  /// idea when it runs next.
  [[nodiscard]] std::uint32_t spare() const noexcept {
    const std::uint32_t stealable = m_stealable.load(std::memory_order_acquire);
    return stealable + (stealable != 0 && m_running_realtime.load(std::memory_order_acquire));
  }

  /// @brief What the idle loop sleeps on: non-zero when there is work.
  [[nodiscard]] const std::atomic<std::uint32_t>* idle_word() const noexcept { return &m_waiting; }

  /// @brief How busy the CPU is, to compare it with others. Fine to be slightly out of date.
  [[nodiscard]] std::uint32_t load() const noexcept {
    return waiting() + (current() != m_idle_thread ? 1 : 0);
  }

  /// @brief Publishes the CPU as idle in its domain.
  void publish_idle() noexcept;

  /// @brief A cheap pseudo-random number, for sampling other CPUs. Not for anything else.
  [[nodiscard]] std::uint64_t next_random() noexcept;

  [[nodiscard]] std::uint32_t cpu_id() const noexcept { return m_cpu_id; }
  [[nodiscard]] std::uint32_t slot() const noexcept { return m_slot; }
  [[nodiscard]] NumaDomain* domain() const noexcept { return m_domain; }

  /// @brief Context switches this CPU went through.
  [[nodiscard]] std::uint64_t switches() const noexcept { return m_switches; }

  /// @brief The fair tier, for looking at. Goes with the lock.
  [[nodiscard]] const EevdfQueue& fair() const noexcept { return m_fair; }
};
}  // namespace kernel::sched
