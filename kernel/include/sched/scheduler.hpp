/// @file scheduler.hpp
/// @brief Deciding what every CPU runs.
///
/// The pieces, from the bottom up:
///
///  - sched::EevdfQueue (eevdf.hpp) orders the ordinary threads of one CPU.
///  - sched::RunQueue (runqueue.hpp) adds real-time threads and the lock. Every CPU has one in its
///    hw::PerCpu.
///  - sched::NumaDomain (numa_domain.hpp) knows which CPUs of a node are idle and which have work
///    to spare. Every node has one in its hw::PerNumaNode.
///  - Scheduler, in this file, ties them together: it places threads on CPUs and switches between
///    them. Deciding where a thread belongs is the job of balance.hpp.
///
/// Above that are the things threads block on, all of them on top of what is under "Blocking"
/// below: sched::WaitQueue (wait_queue.hpp), and what is made from it. Timeouts and sleeps come
/// from hw::timer through sched::Alarm (sleep.hpp), so there is no second list of deadlines here.
///
/// There is no global lock or global queue, and no table sized for the largest machine there could
/// be. A CPU's scheduling state is in its own per-CPU area and a node's in its own memory, so what
/// a CPU does to itself stays on its own cache lines.
///
/// ### Enqueuing a thread
///
/// A thread that becomes runnable goes to the first of these that works out:
///
///  1. The CPU it ran on last, if that is idle: its cache might still be warm.
///  2. An idle CPU that shares a last-level cache with that one, a whole core if there is one, as
///     a thread next to a busy SMT sibling gets a lot less done.
///  3. An idle core on its home node, then any idle CPU there.
///  4. Idle CPUs on the other nodes, nearest first.
///  5. The less busy of two CPUs of its home node. Two random samples are nearly as good as asking
///     everyone, for a fraction of the cache misses.
///
/// Most of this is one atomic operation on a bitmap of the node, which also claims the CPU, so
/// threads that wake at the same moment do not pile onto the same one.
///
/// ### Evening things out
///
/// A CPU with nothing left to run steals before it goes to sleep: from the CPUs it shares the most
/// with first (see balance.hpp), and from other nodes only if the victim has threads to spare, or
/// one that is stuck behind a real-time thread, as moving away from a thread's memory is not free.
/// Each node keeps a bitmap of the CPUs that have something to take, so this costs an idle CPU a
/// look at a bitmap and not a walk over every run queue.
///
/// A sleeping CPU cannot notice anything, so a CPU that ends up with a thread waiting wakes one of
/// them to come and get it, after every switch and every tick. That is one more look at a bitmap,
/// and there is no balancing beyond that: a busy CPU never has to go through the others' queues.
///
/// ### Preemption
///
/// While threads are waiting for a CPU, a one-shot slice timer runs on it. It charges the running
/// thread, and at the end of the interrupt, the thread loses the CPU if something else should
/// have it (see preempt()). Nothing runs on a CPU that has just one thread, so the timer costs
/// nothing there, and nothing at all goes off on an idle CPU unless a thread asked for that.
///
/// Interrupts do not switch threads while the preemption count is up, so a lock held with one of
/// the usual guards stays held by a running thread. The switch then happens the moment the guard
/// lets go, as the next interrupt may be far away. Idle threads are never preempted at all: their
/// loop looks at the queue by itself, and while the system boots they run ordinary code.
///
/// ### Blocking
///
/// A wait has to look like this, in a loop, because a wakeup can arrive at any point of it, or for
/// another reason entirely:
///
/// @code
/// while (true) {
///   add_to_wait_list(thread);  // so that a waker finds it, from here on
///   Scheduler::prepare_to_block();
///   if (condition_is_met()) {
///     Scheduler::cancel_block();
///     break;
///   }
///
///   Scheduler::schedule();
/// }
/// @endcode
///
/// Nothing outside the scheduler should need to write this. sched::WaitQueue (wait_queue.hpp) wraps
/// it, and sched::Mutex, Semaphore, CondVar, Completion and sleep_for() are built on that.
///
/// ### Priority
///
/// A thread that holds a Mutex somebody more important waits for competes at that thread's
/// priority until it lets go. apply_priority() does the moving around, for a thread that may be
/// running on another CPU or waiting on its queue at that moment.
#pragma once

#include <cstdint>
#include <expected>

#include "core/errors.hpp"
#include "sched/numa_domain.hpp"
#include "sched/runqueue.hpp"
#include "sched/thread.hpp"

namespace kernel::sched {
/// @brief Threads to wake, gathered under a lock and woken after letting go of it.
///
/// What a woken thread usually wants first is the lock its waker holds, and waking takes a run
/// queue lock and often an IPI. So a waker holds on to nothing that the threads it wakes need,
/// and a long list does not keep everyone else off that lock.
///
/// A thread on a list stays where it is in memory: see Thread::retain(). It can be on one list at
/// a time, and add() leaves it alone if it is on another. That one gets to it later, which is as
/// good as being on this one.
///
/// @pre Preemption is off from the first add() until wake_all() is done.
class WakeList {
  Thread* m_first{nullptr};
  Thread* m_last{nullptr};

 public:
  constexpr WakeList() noexcept = default;

  WakeList(const WakeList&) = delete;
  WakeList& operator=(const WakeList&) = delete;

  ~WakeList() noexcept { KASSERT(m_first == nullptr); }

  /// @brief Adds @p thread, which goes after everything on the list so far.
  void add(Thread& thread) noexcept;

  /// @brief Wakes everything on the list, in order.
  void wake_all() noexcept;
};

/// @brief The scheduler. See the file comment.
class Scheduler {
  // Everything that takes the CPU away from the running thread comes through here.
  static void switch_away(PickMode mode) noexcept;

 public:
  /// @brief Builds every node's domain, from the CPU registry, and sets up the calling CPU.
  /// @pre Called once, on the BSP, once the registry and the per-node areas exist and the BSP has
  ///      its boot thread.
  static void initialize() noexcept;

  /// @brief Sets up the calling CPU's run queue, around the thread it is running.
  /// @pre initialize() is done and the CPU has its boot thread, but is not online yet.
  static void initialize_cpu() noexcept;

  /// @brief Takes in what can only be known once all CPUs are up: how they compare to each other.
  /// @pre Called once, on the BSP, after the last of them came online.
  static void finish_bring_up() noexcept;

  /// @brief Whether initialize() is done.
  [[nodiscard]] static bool is_initialized() noexcept;

  /// @brief Hands a new thread to the scheduler. It runs from now on, and goes away by itself
  ///        when it is done, so the pointer must not be used after that.
  /// @return Error::InvalidArgument for null or an idle thread, Error::InvalidState if the thread
  ///         is not new, if the CPU it is pinned to is not online, or before initialize().
  [[nodiscard]] static auto enqueue(Thread* thread) noexcept -> std::expected<void, Error>;

  /// @brief Makes a blocked thread runnable again. Safe from interrupt handlers.
  /// @pre @p thread is not gone yet, which is up to whatever list the caller found it on.
  /// @return false if it was not blocked, in which case nothing has happened.
  static bool wake(Thread* thread) noexcept;

  /// @brief Makes @p thread compete with @p priority from now on, wherever it is, and gets the
  ///        attention of its CPU if that changes what should be running there.
  ///
  /// An idle thread stays as it is. It only runs when nothing else does, whatever it is worth.
  /// @pre @p thread is not gone yet.
  static void apply_priority(Thread& thread, Priority priority) noexcept;

  /// @brief Announces that the calling thread is about to block. The next schedule() takes it off
  ///        the CPU, unless a wake() gets in first. See the file comment.
  static void prepare_to_block() noexcept;

  /// @brief Takes back prepare_to_block(), when it turned out there was no need to wait.
  static void cancel_block() noexcept;

  /// @brief Runs whatever should be running now, which may well be the caller.
  ///
  /// Comes back when the caller gets a CPU again. Any CPU: per-CPU pointers taken before the call
  /// are stale after it.
  static void schedule() noexcept;

  /// @brief Lets others go first, if there are any. Real-time threads only yield to their equals.
  static void yield() noexcept;

  /// @brief Ends the calling thread. What Thread does when its entry function returns.
  [[noreturn]] static void exit_current() noexcept;

  /// @brief What every way back to user mode goes through: whatever the AST flags ask for, and
  ///        then the end of the thread if someone asked it to exit.
  ///
  /// A user thread can only be stopped from the outside here. It has no locks and nothing else
  /// half done once it is on its way out to ring 3, and every interrupt that takes it back into
  /// the kernel leaves through here, so a thread that is asked to exit is gone the next time
  /// anything interrupts it.
  /// @pre Interrupts are off, and the calling thread is about to go to ring 3.
  static void return_to_user() noexcept;

  /// @brief Asks @p thread to exit (Thread::request_exit()), and sees that it hears soon.
  ///
  /// The flag alone is not enough. A thread running in ring 3 on another CPU only comes into the
  /// kernel when something interrupts it, and on a CPU with nothing else to run no slice timer is
  /// armed, so that may be never. A reschedule IPI brings it in, and return_to_user() ends it on
  /// the way back. A thread that is not running goes through there when it next runs.
  static void request_exit(Thread& thread) noexcept;

  /// @brief What an idle thread does for the rest of its life.
  [[noreturn]] static void enter_idle_loop() noexcept;

  /// @brief One pass of the idle loop: clears up, runs what there is, tries to steal some work,
  ///        and otherwise sleeps until something happens.
  ///
  /// For code that has a CPU's idle thread and wants to do something else in between, the way the
  /// test kernel's APs wait for their next test.
  /// @pre The caller is the CPU's idle thread.
  static void idle_step() noexcept;

  /// @brief Does the accounting of an interrupt: charges the running thread, decides whether it
  ///        should give way, and keeps the slice timer running.
  /// @pre In interrupt context, with interrupts off.
  static void tick() noexcept;

  /// @brief Takes the CPU away from the running thread, if it is the kind that can lose it.
  static void preempt() noexcept;

  /// @brief Acts on the AST flags: runs deferred calls, then takes the CPU away if that is wanted.
  ///        Called when hw::percpu::ast_pending() says so, on the way out of an interrupt or of a
  ///        section with preemption off.
  /// @pre Interrupts are off.
  static void handle_ast() noexcept;

  /// @brief Completes a context switch on the thread that was switched to: releases the lock that
  ///        the previous thread left it. Does nothing after a switch_to() by hand.
  static void finish_switch() noexcept;

  /// @brief The CPU a thread should go to right now. See the file comment.
  ///
  /// Claims it if it is idle, so calling this means the thread had better go there.
  [[nodiscard]] static std::uint32_t select_target_cpu(const Thread* thread) noexcept;

  /// @brief The run queue of @p cpu_id, which has to exist.
  [[nodiscard]] static RunQueue& cpu_runqueue(std::uint32_t cpu_id) noexcept;

  /// @brief The domain of @p node_id, which has to exist.
  [[nodiscard]] static NumaDomain& numa_domain(std::uint32_t node_id) noexcept;
};
}  // namespace kernel::sched
