/// @file mutex.hpp
/// @brief A lock that sleeps, and that lends its owner the priority of whoever waits for it.
///
/// @code
/// sched::Mutex lock;
/// {
///   utils::NakedGuard guard{lock};
///   ...  // can sleep, take page faults, and so on
/// }
/// @endcode
///
/// A spinlock costs the CPU of everyone waiting. This costs them nothing, but cannot be taken in an
/// interrupt handler or with preemption off, and is a poor deal for a handful of instructions.
///
/// ### Nobody in the way
///
/// All there is to the state is one word: the owner's Thread pointer, and two flags in the low bits
/// that a thread's alignment leaves free. Locking and unlocking are one atomic operation each, as
/// long as no thread is in line.
///
/// ### Getting in line
///
/// If the owner is running on another CPU, it will likely be done soon, so a thread watches for a
/// while first. That takes no context switch, which is worth a lot since most critical sections
/// are short. If that does not work out, it goes in line. The line is sorted by priority, and
/// threads of equal priority keep the order they came in.
///
/// ### Who gets it next
///
/// Unlocking wakes the first in line, but does not hand it the lock. It takes a while for that
/// thread to get a CPU, and a thread that is running can take the lock in the meantime, which is
/// how a contended lock keeps moving. The price is that a woken thread may find it gone, so going
/// ahead is limited two ways:
///
///  - Only a thread at least as important as the first in line can do it. A low priority thread
///    that happens to be running cannot delay a high priority one.
///  - The first in line can only lose this way once. After that it says so (`HANDOFF`), and the
///    next unlock gives it the lock outright.
///
/// ### Priority inheritance
///
/// With a low priority thread holding what a high priority one needs, anything in between would
/// keep both from running. So the owner of a lock competes with the priority of the first in line,
/// if that is more than it has by itself, for as long as it holds the lock.
///
/// That takes more than remembering one number. A thread can hold several contended locks, so
/// it gets the best of what they all offer, and after unlocking one it falls back to what the rest
/// are worth. And an owner can itself be in line for another lock, so what it inherits has to be
/// passed on to that lock's owner, and so on down the chain (see propagate()). Going down the
/// chain can find a deadlock too, if it leads back to where it started with nobody on the way
/// having a time limit. That is a bug in the caller, and the end of the kernel. It only goes as far
/// as priorities change, though, so getting past this is no proof of anything.
///
/// Only Mutex does this. A Semaphore or CondVar has no owner to lend anything to.
///
/// ### What holds this together
///
/// With m_wait_lock free, there is a line exactly if `WAITERS` is set. Nearly everything relies on
/// it: with the flag set, the owner cannot change without going through m_wait_lock, and without
/// it, no one needs to look at the line to take or release the lock.
///
/// ### Locks
///
/// In the order they are taken: Mutex::m_wait_lock, then Thread::m_pi_lock, then a run queue. None
/// of them is used from interrupt handlers, so holding them with preemption off is enough.
/// Going down a chain has to go from a thread to the next mutex, against that order, which only
/// ever uses try_lock() and backs off.
///
/// Do not put a Mutex in the object it protects if unlocking it may be the last thing that happens
/// to the object: unlock() can still be touching the Mutex after the next owner has it. That is
/// what sched::Completion is for.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "sched/priority.hpp"
#include "sched/thread.hpp"
#include "utils/avl_tree.hpp"
#include "utils/linked_list.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::sched {
/// @brief A thread's place in line for a Mutex. Lives on its stack.
struct MutexWaiter : utils::AvlNode {
  MutexWaiter(Thread* owner, Mutex* wanted, const bool can_give_up) noexcept
      : thread(owner), mutex(wanted), has_time_limit(can_give_up) {}

  Thread* thread;
  Mutex* mutex;
  bool has_time_limit;  ///< It leaves by itself sooner or later, whatever else happens.
  bool granted{false};  ///< The last owner gave it the lock, see the file comment.
  Priority priority{};  ///< The order in line. Changes only while out of the tree.
};

/// @brief A sleeping lock with priority inheritance. See the file comment.
class Mutex : public utils::SListNode<Mutex, HeldMutexTag> {
  static constexpr std::uintptr_t WAITERS = 1u << 0;  ///< Unlocking has to look at the line.
  static constexpr std::uintptr_t HANDOFF = 1u << 1;  ///< The next owner is the first in line.
  static constexpr std::uintptr_t FLAGS = WAITERS | HANDOFF;

  struct Order {
    [[nodiscard]] static bool less(const MutexWaiter& a, const MutexWaiter& b) noexcept {
      return a.priority > b.priority;
    }

    static void update(MutexWaiter&) noexcept {}
  };

  using Line = utils::AvlTree<MutexWaiter, Order>;

  std::atomic<std::uintptr_t> m_state{0};

  // Where the owner took the lock, for whoever is deciding whether to wait for it. All this is good
  // for is telling that the owner is running, and it is out of date at worst: see spin_on_owner().
  std::atomic<std::uint32_t> m_owner_cpu{0};

  utils::QSpinlock m_wait_lock{};
  Line m_line{};
  Priority m_donation{};  ///< What it lends its owner, while it is on the owner's list.

  [[nodiscard]] static Thread* owner_of(const std::uintptr_t state) noexcept {
    return reinterpret_cast<Thread*>(state & ~FLAGS);
  }

  [[nodiscard]] static std::uintptr_t word_of(const Thread* thread) noexcept {
    return reinterpret_cast<std::uintptr_t>(thread);
  }

  void note_owner_cpu() noexcept { m_owner_cpu.store(hw::percpu::id(), std::memory_order_relaxed); }

  [[nodiscard]] bool spin_on_owner() noexcept;
  bool lock_slow(const klib::TimePoint* deadline) noexcept;
  void unlock_slow() noexcept;

  [[nodiscard]] bool try_take_locked(MutexWaiter& waiter) noexcept;
  void settle_owner_locked(MutexWaiter& waiter) noexcept;
  [[nodiscard]] bool stand_in_line_locked(MutexWaiter& waiter) noexcept;
  void leave_line_locked(MutexWaiter& waiter) noexcept;
  void clear_flags() noexcept;

  static bool refresh_priority(Thread& thread) noexcept;
  static void propagate(Mutex* mutex, const Thread* deadlock_on) noexcept;

  template <typename Retain>
  [[nodiscard]] static Mutex* next_in_chain(Thread* owner, const Thread*& deadlock_on,
                                            Retain&& retain) noexcept;

 public:
  constexpr Mutex() noexcept = default;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  /// @brief Takes the lock, however long it takes.
  /// @pre The calling thread can sleep (see hw::percpu::can_sleep()) and does not hold the lock.
  void lock() noexcept {
    std::uintptr_t expected = 0;
    if (m_state.compare_exchange_strong(expected, word_of(hw::percpu::current_thread()),
                                        std::memory_order_acquire, std::memory_order_relaxed))
        [[likely]] {
      note_owner_cpu();
      return;
    }

    (void)lock_slow(nullptr);
  }

  /// @brief Takes the lock if nobody has it and nobody is in line. Never waits.
  /// @pre The calling thread is not in an interrupt handler: it has no business owning anything.
  [[nodiscard]] bool try_lock() noexcept {
    std::uintptr_t expected = 0;
    if (!m_state.compare_exchange_strong(expected, word_of(hw::percpu::current_thread()),
                                         std::memory_order_acquire, std::memory_order_relaxed)) {
      return false;
    }

    note_owner_cpu();
    return true;
  }

  /// @brief Takes the lock, unless that takes longer than @p timeout.
  /// @return Whether it has the lock now. @pre As for lock().
  [[nodiscard]] bool try_lock_for(klib::Duration timeout) noexcept;

  /// @brief Lets go of the lock.
  /// @pre The calling thread holds it. Not necessarily from the same CPU it took it on.
  void unlock() noexcept {
    std::uintptr_t expected = word_of(hw::percpu::current_thread());
    if (!m_state.compare_exchange_strong(expected, 0, std::memory_order_release,
                                         std::memory_order_relaxed)) [[unlikely]] {
      unlock_slow();
    }
  }

  /// @brief Who holds the lock. Only tells the holder anything for sure.
  [[nodiscard]] Thread* owner() const noexcept {
    return owner_of(m_state.load(std::memory_order_relaxed));
  }
};
}  // namespace kernel::sched
