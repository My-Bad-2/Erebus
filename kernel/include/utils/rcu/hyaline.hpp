/// @file hyaline.hpp
/// @brief The insides of RCU reclamation: batches, and what each CPU keeps of them.
///
/// A CPU fills one batch at a time with what it retires. Sealing it writes down the readers it has
/// to wait for (Waiter), and the batch joins the CPU's `waiting` queue. Once all of those readers
/// are gone it moves to `ready`, or is reclaimed on the spot when it holds nothing but frees and
/// nothing is ahead of it. Both queues keep the order batches were sealed in, and so does
/// reclamation: that is what makes `completed` a count of every batch up to some point, which is
/// what rcu::barrier() waits on.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "hal/timer/timer.hpp"
#include "sched/dpc.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::utils::rcu::detail {
/// @brief One retired thing: `fn(arg)`, or `kfree(arg)` when there is no `fn`.
///
/// No default values, here or in Waiter, so that a new RetireBatch can leave its arrays alone.
struct Entry {
  RetireFn fn;
  void* arg;
};

/// @brief A reader a batch waits for: the CPU, and its PerCpu::rcu_reader as the batch saw it.
///
/// The reader is gone once anything above the depth byte has changed, as that only happens when
/// the CPU finishes an outermost section. It never changes back, so a waiter that is done stays
/// done.
struct Waiter {
  std::uint32_t cpu;
  std::uint64_t reader;
};

/// @brief Up to CAPACITY retired things, and who they wait for.
///
/// Made with default-initialization (`new (memory) RetireBatch`), which sets the header and leaves
/// `entries` and `inline_waiters` as they were: only those below `count` and `waiter_count` are
/// ever read, and each is written first. Zeroing them would be most of what a new batch costs,
/// nearly a kilobyte of stores on the way to every seal.
struct alignas(std::hardware_destructive_interference_size) RetireBatch {
  static constexpr std::uint16_t CAPACITY = 48;

  /// Readers a batch can wait for without an allocation. More than this many CPUs in a read
  /// section at the moment one batch is sealed is a busy machine; it gets an array of its own.
  static constexpr std::uint32_t INLINE_WAITERS = 6;

  RetireBatch* next{nullptr};  ///< In a queue, or on the way to being freed.
  std::uint64_t sequence{0};   ///< Its place among the batches of its CPU, from 1.
  std::uint16_t count{0};
  std::uint16_t callbacks{0};  ///< Entries with a function, which can only run in a DPC.
  std::uint32_t waiter_count{0};
  Waiter* waiters{nullptr};  ///< inline_waiters, or an array from the heap.

  std::array<Entry, CAPACITY> entries;
  std::array<Waiter, INLINE_WAITERS> inline_waiters;

  [[nodiscard]] bool empty() const noexcept { return count == 0; }
  [[nodiscard]] bool full() const noexcept { return count == CAPACITY; }
  [[nodiscard]] bool frees_only() const noexcept { return callbacks == 0; }
};

/// @brief Batches in the order they were sealed.
struct BatchQueue {
  RetireBatch* head{nullptr};
  RetireBatch* tail{nullptr};

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

  void push_back(RetireBatch* batch) noexcept {
    batch->next = nullptr;
    (tail != nullptr ? tail->next : head) = batch;
    tail = batch;
  }

  RetireBatch* pop_front() noexcept {
    RetireBatch* batch = head;
    if (batch != nullptr) {
      head = batch->next;
      if (head == nullptr) {
        tail = nullptr;
      }

      batch->next = nullptr;
    }

    return batch;
  }
};

/// @brief What one CPU keeps. See the file comment for who touches what.
struct alignas(std::hardware_destructive_interference_size) CpuState {
  RetireBatch* current{nullptr};  ///< Being filled. Null if allocating one failed.
  RetireBatch* spare{nullptr};    ///< So that sealing seldom has to wait for the slab allocator.
  BatchQueue waiting{};           ///< Sealed, with readers left to wait for.
  BatchQueue ready{};             ///< Done waiting, with callbacks to run.
  bool reclaiming{false};         ///< A DPC is running a batch it took off `ready`.
  std::uint64_t sealed{0};        ///< Batches sealed so far.

  hw::timer::Timer timer{};  ///< Seals and looks again while anything waits.
  sched::Dpc dpc{};          ///< Runs what is on `ready`.

  // Written by the CPU alone, and read by anyone.
  std::atomic<std::uint64_t> completed{0};  ///< Batches reclaimed, all of them up to this one.
  std::atomic<std::uint64_t> retired{0};
  std::atomic<std::uint64_t> reclaimed{0};
  std::atomic<std::uint64_t> fastpath_batches{0};
  std::atomic<std::uint64_t> hyaline_batches{0};
  std::atomic<std::uint64_t> synchronizes{0};
};
}  // namespace kernel::utils::rcu::detail
