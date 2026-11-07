/// @file reaper.hpp
/// @brief `kreaperd`, which destroys threads once they are done.
///
/// A thread cannot free its own stack, as it is standing on it, and the CPU that switched away
/// from it is in the middle of a context switch, which is no place for the work either: unmapping
/// a stack takes the kernel address space lock and a TLB shootdown that waits for every other
/// CPU. So a dead thread goes from its run queue to here, and a Background thread does it later.
///
/// The hand-over happens in Scheduler::finish_switch(), on the thread that was switched to, once
/// the run queue lock that was held through the switch is gone. From that moment nothing is on
/// the dead thread's stack, which is what Thread::destroy() needs to know.
///
/// Most stacks never get unmapped at all: Thread keeps a few for the next threads to come along,
/// and only what does not fit there goes back to the VMM (see Thread::stack_cache_stats()).
#pragma once

#include <cstdint>

#include "sched/thread.hpp"
#include "utils/linked_list.hpp"

namespace kernel::sched::reaper {
/// @brief Starts `kreaperd`. Threads that die before this wait for it.
/// @pre The scheduler runs. Called once.
void initialize() noexcept;

/// @brief Hands over @p dead, which ends up empty. Safe with interrupts off, from any thread.
/// @pre Every thread on it is `Dead` and has left its CPU for good.
void enqueue_dead(utils::SList<Thread, ReapQueueTag>& dead) noexcept;

/// @brief enqueue_dead() for a single thread.
void enqueue_dead(Thread* thread) noexcept;

/// @brief How many threads `kreaperd` has destroyed so far.
[[nodiscard]] std::uint64_t reaped_count() noexcept;

/// @brief How many it has been handed and not got to yet. Out of date as soon as it is read.
[[nodiscard]] std::uint32_t pending_count() noexcept;
}  // namespace kernel::sched::reaper
