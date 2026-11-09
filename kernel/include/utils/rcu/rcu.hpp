/// @file rcu.hpp
/// @brief Read-copy-update: lock-free readers, and memory that is freed once no reader can still
///        be looking at it.
///
/// @code
/// rcu::Ptr<Route> g_route;
///
/// // Reader, in any context, NMIs included: no lock, and no cache line shared with anyone.
/// {
///   rcu::ReadGuard rcu;
///   if (const Route* route = g_route.load(rcu)) {
///     use(route->gateway);
///   }
/// }
///
/// // Writer, serialised against other writers by a lock of its own:
/// g_route.replace_and_retire(new Route{...});
/// @endcode
///
/// A writer never frees what it unlinked itself, because a reader may be halfway through it. It
/// retires it instead, and the object is freed once every reader that could have seen it is gone.
/// Readers that come along later cannot find it anymore, so they do not count.
///
/// ### Readers
///
/// A read section is a ReadGuard. It keeps the thread on its CPU (no preemption, so no sleeping
/// either) and marks the CPU as reading in PerCpu::rcu_reader: the nesting depth in the low byte,
/// and above it the number of outermost sections the CPU has finished. Entering is one locked
/// increment of that word, leaving one plain update, both on a cache line nobody else writes.
///
/// The locked increment is not optional. A writer decides whom to wait for by looking at that
/// word after unlinking, and on x86 a plain store can sit in the store buffer while the loads after
/// it already run: the reader would find the old pointer while the writer still saw it idle, and
/// free the object under it. A locked instruction drains the store buffer first, and the writer's
/// fence does the same on its side, so one of the two always sees the other.
///
/// Read sections nest, up to 255 deep, and work in interrupt handlers and NMIs like anywhere else:
/// an interrupt can only add sections of its own, which are over by the time it returns. Every
/// interrupt is in one already, as the interrupt manager dispatches them in a read section (see
/// hal/interrupts/interrupt_manager.hpp). So a CPU is a reader for as long as it handles one, and
/// writers wait for that like for any other section.
///
/// ### Writers
///
/// Retired objects collect in a batch per CPU, outside the objects themselves, so nothing needs an
/// `rcu_head` of its own. When the batch is full, or has waited a moment, it is sealed: the sealer
/// looks at every online CPU and writes down the ones that are in a read section, and how many
/// sections each has finished. That is everyone the batch has to wait for, and the wait ends for a
/// CPU as soon as that number moves. This is the Hyaline idea, specialized for a kernel:
///
///  - No reader anywhere, which is the common case, and the batch has nothing to wait for.
///  - Otherwise it waits for exactly those readers, not for every CPU in the system to pass a
///    quiescent state, the way classic RCU does. An idle CPU, or one that is busy elsewhere, holds
///    nothing up.
///  - Readers are never asked to do anything. Whether the readers a batch waits for are done is
///    something the writer's CPU can see for itself, so a reader that went idle, or runs user code
///    for an hour, cannot keep memory from being freed.
///
/// The owning CPU looks again whenever it seals, and a timer makes sure it does at least every
/// millisecond while anything waits, so a retired object is freed within a couple of milliseconds
/// of its last reader leaving, and a lone retirement does not wait for a batch to fill up. A batch
/// sealed in an interrupt waits for that interrupt at least; the CPU's deferred call looks at it
/// again as soon as the interrupt is over.
///
/// ### What runs where
///
/// Plain frees (retire(), retire_raw()) happen wherever the batch completes, interrupt handlers
/// included: kfree() is fine there. Callbacks (call_after_grace_period(), and retire() of types
/// with a destructor) run in a deferred call on the retiring CPU (sched/dpc.hpp): interrupts on,
/// no sleeping, never inside the code that retired them, so they can take the locks it held.
/// Everything a CPU retires is reclaimed in the order it was retired.
///
/// ### Where the kernel uses it
///
///  - The handlers of each interrupt vector, which every interrupt and exception looks up
///    (hal/interrupts/interrupt_manager.hpp).
///  - The list of every heap cache, which shrinks walk while caches come and go (memory/heap.hpp).
///  - The shrinker registry, which reclaim walks while calling shrinkers that sleep, in a sleepable
///    domain (memory/shrinker.hpp).
///  - Page faults, which find their VMA and resolve the fault in a read section, without the
///    address space's lock, while no writer is about (memory/vmm/vma/vma.hpp).
///
/// ### Compared to the alternatives
///
/// Classic RCU gets its readers for free, but finding out that every CPU has passed a quiescent
/// state needs hooks in the scheduler, the idle loop and the way back to user mode, and takes
/// milliseconds at best. Here readers pay one locked instruction on a private cache line, and the
/// update side needs nothing from the rest of the kernel. Sleeping readers are the business of
/// rcu::SleepableDomain (srcu.hpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "hal/smp/percpu.hpp"
#include "memory/heap.hpp"
#include "utils/assert.hpp"

namespace kernel::utils::rcu {
/// @brief What call_after_grace_period() runs.
using RetireFn = void (*)(void* arg) noexcept;

/// @brief Sets up the state of every CPU. Before this, retiring frees right away, which is only
///        safe because nothing but the BSP runs then.
/// @pre The heap works. Called once, on the BSP.
void initialize() noexcept;

/// @brief Says that the calling CPU is somewhere no read section can be going on: in a context
///        switch, or about to idle.
///
/// Nothing has to happen there. Readers are tracked by their own words (see the file comment), so
/// no grace period waits for CPUs to pass such points, and memory is not reclaimed here either:
/// the scheduler calls this with its run queue lock held. What is left is a check, in builds with
/// assertions, that the CPU really is outside every read section. A thread that blocks or sleeps
/// in one is the bug it catches. This function is essentially a no-op.
[[gnu::always_inline]] inline void note_quiescent_state() noexcept {
  KASSERT(!hw::percpu::in_rcu_read());
}

/// @brief A read section. See the file comment.
///
/// No sleeping, no blocking on a Mutex and no switching threads inside one: the thread may not
/// leave its CPU until the guard goes. Spinlocks are fine.
class [[nodiscard]] ReadGuard {
 public:
  [[gnu::always_inline]] ReadGuard() noexcept {
    KASSERT(hw::percpu::rcu_depth() < hw::percpu::RCU_DEPTH_MASK);
    hw::percpu::rcu_read_lock();
  }

  [[gnu::always_inline]] ~ReadGuard() noexcept { hw::percpu::rcu_read_unlock(); }

  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;
  ReadGuard(ReadGuard&&) = delete;
  ReadGuard& operator=(ReadGuard&&) = delete;
};

/// @brief Hands @p ptr to memory::heap::kfree() once every reader that might see it is gone.
///
/// Never blocks and never fails: without memory for a batch it waits for the readers right there.
/// @pre @p ptr came from the heap (kmalloc(), a KmemCache or `new`) and is unreachable for new
///      readers. Not from an NMI.
void retire_raw(void* ptr) noexcept;

/// @brief Calls `fn(arg)` once every reader that is in a section now is gone. See the file comment
///        for the context it runs in.
/// @pre Not from an NMI.
void call_after_grace_period(RetireFn fn, void* arg) noexcept;

/// @brief Retires an object that came from the heap: destroys and frees it after one grace period.
///
/// Types without a destructor go straight to kfree(), the others through a callback that runs the
/// destructor first. Null is fine.
template <typename T>
void retire(T* ptr) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return;
  }

  auto* object = const_cast<std::remove_cv_t<T>*>(ptr);
  if constexpr (std::is_trivially_destructible_v<T>) {
    retire_raw(object);
  } else {
    call_after_grace_period(
        [](void* raw) noexcept {
          auto* doomed = static_cast<std::remove_cv_t<T>*>(raw);
          std::destroy_at(doomed);
          memory::heap::kfree(doomed);
        },
        object);
  }
}

/// @brief Seals what the calling CPU has retired so far, instead of letting it wait for the batch
///        to fill up or for the timer.
void flush_local_batch() noexcept;

/// @brief Waits until every read section that is running anywhere right now is over.
///
/// For the classic pattern of unlinking, waiting and freeing by hand. Read sections are short by
/// construction, so it spins, and answers IPIs all the while, as a reader may be waiting for this
/// CPU. A caller that can sleep lets other threads have the CPU meanwhile; one that cannot, a
/// writer under a spinlock say, only spins.
/// @pre The caller is not in a read section, which would wait for itself. Interrupt handlers are.
void synchronize() noexcept;

/// @brief synchronize(). Every grace period here is as quick as the readers it waits for, so there
///        is nothing to expedite; this is for code written against the usual RCU interface.
inline void synchronize_expedited() noexcept { synchronize(); }

/// @brief Waits until everything retired anywhere before the call has been reclaimed: freed, or
///        its callback run.
///
/// What code needs before it goes away while callbacks of its own may still be queued.
/// @pre The calling thread can sleep.
void barrier() noexcept;

/// @brief Reclaims whatever the calling CPU has that is ready, callbacks included, right here.
/// @pre The calling thread can sleep.
/// @return How many objects and callbacks that was.
std::size_t drain_reclaimable() noexcept;

/// @brief What RCU has done so far, all CPUs together. Out of date by the time it is read.
struct Stats {
  std::uint64_t objects_retired{0};    ///< Objects and callbacks handed over.
  std::uint64_t objects_reclaimed{0};  ///< Of those, freed or called.
  std::uint64_t fastpath_batches{0};   ///< Sealed with nobody reading: nothing to wait for.
  std::uint64_t hyaline_batches{0};    ///< Sealed with readers to wait for.
  std::uint64_t synchronizes{0};       ///< Calls to synchronize() that finished.
};

[[nodiscard]] Stats stats() noexcept;
}  // namespace kernel::utils::rcu
