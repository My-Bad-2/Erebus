#include "utils/rcu/hyaline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono.hpp>
#include <memory>
#include <new>
#include <utility>

#include "hal/hal.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::utils::rcu {
namespace {
using detail::CpuState;
using detail::Entry;
using detail::RetireBatch;
using detail::Waiter;

constexpr klib::Duration POLL_PERIOD = klib::Duration::milliseconds(1);
constexpr klib::Duration POLL_SLACK = klib::Duration::milliseconds(1);
constexpr std::uint32_t SPINS_PER_YIELD = 64;

memory::heap::KmemCache* g_batch_cache = nullptr;
CpuState* g_states = nullptr;
std::uint32_t g_state_count = 0;

[[nodiscard]] CpuState& local_state() noexcept { return g_states[hw::percpu::id()]; }

// The counters are written by their CPU alone, with interrupts off or preemption off, so a plain
// update is enough. They are atomic for the CPUs that read them.
void bump(std::atomic<std::uint64_t>& counter, const std::uint64_t by = 1) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

// PerCpu::rcu_reader of @p cpu. Its CPU updates it with plain instructions; an aligned load of
// all eight bytes still sees one whole value or the next.
[[nodiscard]] std::uint64_t reader_word(const std::uint32_t cpu) noexcept {
  return __atomic_load_n(&hw::percpu::cpu_area(cpu)->rcu_reader, __ATOMIC_ACQUIRE);
}

[[nodiscard]] constexpr bool is_reading(const std::uint64_t word) noexcept {
  return (word & hw::percpu::RCU_DEPTH_MASK) != 0;
}

// Whether a CPU whose word was @p seen has finished that outermost section since.
[[nodiscard]] constexpr bool has_moved_on(const std::uint64_t now,
                                          const std::uint64_t seen) noexcept {
  return now / hw::percpu::RCU_SECTION_DONE != seen / hw::percpu::RCU_SECTION_DONE;
}

// Spins until the section @p cpu was in when its word read @p seen is over. Keeps answering IPIs,
// as the reader may be waiting for one from here, and gives the CPU away now and then if it can.
void wait_for_reader(const std::uint32_t cpu, const std::uint64_t seen) noexcept {
  for (std::uint32_t spins = 1; !has_moved_on(reader_word(cpu), seen); ++spins) {
    hw::ipi::poll();
    hw::cpu_relax();

    if (spins % SPINS_PER_YIELD == 0 && hw::percpu::can_sleep()) {
      sched::Scheduler::yield();
    }
  }
}

// Waits for every read section that is running now, the calling CPU's included. The caller must
// not be in one, or this would wait for itself.
void wait_for_all_readers() noexcept {
  // Whatever the caller unlinked is visible everywhere before any reader's word is looked at.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // Every CPU, this one too: a thread that moves while it looks must not skip the CPU it started
  // on.
  hw::smp::for_each_online([](const std::uint32_t cpu) {
    if (const std::uint64_t word = reader_word(cpu); is_reading(word)) {
      wait_for_reader(cpu, word);
    }
  });

  std::atomic_thread_fence(std::memory_order_seq_cst);
}

[[nodiscard]] RetireBatch* new_batch(CpuState& st) noexcept {
  // Not std::construct_at(), which would zero the arrays as well. See RetireBatch.
  if (RetireBatch* spare = std::exchange(st.spare, nullptr)) {
    return ::new (static_cast<void*>(spare)) RetireBatch;
  }

  const auto memory = g_batch_cache->alloc();
  return memory ? ::new (*memory) RetireBatch : nullptr;
}

void recycle(CpuState& st, RetireBatch* batch) noexcept {
  if (batch->waiters != nullptr && batch->waiters != batch->inline_waiters.data()) {
    memory::heap::kfree(batch->waiters);
  }

  std::destroy_at(batch);
  if (st.spare == nullptr) {
    st.spare = batch;
  } else {
    g_batch_cache->free(batch);
  }
}

// Room for at least one more waiter, on the heap. @return false without memory.
[[nodiscard]] bool grow_waiters(RetireBatch& batch, std::uint32_t& capacity) noexcept {
  const std::uint32_t wanted = std::max(hw::smp::online_cpus(), capacity * 2);
  auto* bigger =
      static_cast<Waiter*>(memory::heap::kmalloc(sizeof(Waiter) * wanted, alignof(Waiter)));
  if (bigger == nullptr) [[unlikely]] {
    return false;
  }

  std::uninitialized_copy_n(batch.waiters, batch.waiter_count, bigger);
  if (batch.waiters != batch.inline_waiters.data()) {
    memory::heap::kfree(batch.waiters);
  }

  batch.waiters = bigger;
  capacity = wanted;
  return true;
}

// Writes down every reader @p batch has to wait for. The batch is nobody else's yet, so interrupts
// can be on, as long as the thread stays on its CPU.
// @return Whether the calling CPU is one of them.
[[nodiscard]] bool find_readers(RetireBatch& batch) noexcept {
  // The other half of what makes this work is the locked increment in hw::percpu::rcu_read_lock().
  std::atomic_thread_fence(std::memory_order_seq_cst);

  std::uint32_t capacity = RetireBatch::INLINE_WAITERS;
  batch.waiters = batch.inline_waiters.data();
  batch.waiter_count = 0;

  // This CPU first. It may be retiring from inside a read section of its own, and that is the one
  // reader nobody here could wait for, so it must not be the one that finds no room.
  const std::uint32_t me = hw::percpu::id();
  const std::uint64_t mine = reader_word(me);
  const bool me_too = is_reading(mine);
  if (me_too) {
    batch.waiters[batch.waiter_count++] = Waiter{.cpu = me, .reader = mine};
  }

  hw::smp::for_each_online([&](const std::uint32_t cpu) {
    const std::uint64_t word = cpu != me ? reader_word(cpu) : 0;
    if (!is_reading(word)) {
      return;
    }

    if (batch.waiter_count == capacity && !grow_waiters(batch, capacity)) [[unlikely]] {
      // Nowhere to write it down: wait for it here instead. It is in a read section, which ends.
      wait_for_reader(cpu, word);
      return;
    }

    batch.waiters[batch.waiter_count++] = Waiter{.cpu = cpu, .reader = word};
  });

  return me_too;
}

// Whether every reader @p batch waits for is gone. Forgets those that are, so the next look is
// shorter: a waiter that is done stays done.
[[nodiscard]] bool readers_gone(RetireBatch& batch) noexcept {
  for (std::uint32_t i = 0; i < batch.waiter_count;) {
    const Waiter& waiter = batch.waiters[i];
    if (has_moved_on(reader_word(waiter.cpu), waiter.reader)) {
      batch.waiters[i] = batch.waiters[--batch.waiter_count];
    } else {
      ++i;
    }
  }

  return batch.waiter_count == 0;
}

// Frees and calls everything in @p batch, in the order it was retired.
std::size_t run_entries(const RetireBatch& batch) noexcept {
  for (std::uint16_t i = 0; i < batch.count; ++i) {
    const Entry& entry = batch.entries[i];
    if (entry.fn != nullptr) {
      entry.fn(entry.arg);
    } else {
      memory::heap::kfree(entry.arg);
    }
  }

  return batch.count;
}

void finish(CpuState& st, RetireBatch* batch) noexcept {
  bump(st.reclaimed, batch->count);
  st.completed.store(batch->sequence, std::memory_order_release);
  recycle(st, batch);
}

// Moves along whatever is done waiting, in order. Frees are fine anywhere, so a batch of nothing
// but frees goes right away, unless something is ahead of it. Callbacks, and whatever is behind
// them, are for the DPC.
// @pre Interrupts are off, on the CPU that owns @p st, which is not in an NMI.
std::size_t advance(CpuState& st) noexcept {
  std::size_t reclaimed = 0;

  while (!st.waiting.empty() && readers_gone(*st.waiting.head)) {
    RetireBatch* batch = st.waiting.pop_front();

    if (batch->frees_only() && st.ready.empty() && !st.reclaiming) {
      reclaimed += run_entries(*batch);
      finish(st, batch);
    } else {
      st.ready.push_back(batch);
    }
  }

  if (!st.ready.empty() && !st.reclaiming) {
    (void)st.dpc.queue();
  }

  return reclaimed;
}

// Makes sure the CPU looks again soon. Not before the CPU has its timers, which is when it comes
// online: until then, everything waits for the next seal or flush.
// @pre Interrupts are off, on the CPU that owns @p st.
void arm_timer(CpuState& st) noexcept {
  if (!st.timer.is_pending() && hw::smp::is_online(hw::percpu::id())) {
    (void)st.timer.start_after(POLL_PERIOD, POLL_SLACK);
  }
}

// Seals @p batch, which the calling CPU has just taken out of its `current`, and takes it as far
// as it can go right now.
// @pre The thread stays on its CPU, which owns @p st. Interrupts may be on.
void seal(CpuState& st, RetireBatch* batch) noexcept {
  // In an interrupt, this CPU is always a reader: interrupts are dispatched in a read section
  // (hal/interrupts/interrupt_manager.hpp), and the timer, barrier()'s IPI and a handler that
  // retires all seal from one. That section is over by the time the DPC runs, so the DPC looks
  // again then, rather than leaving it to the timer a millisecond later.
  const bool waits_for_me = find_readers(*batch);

  IrqDisableGuard irq_guard;
  batch->sequence = ++st.sealed;
  bump(batch->waiter_count == 0 ? st.fastpath_batches : st.hyaline_batches);

  st.waiting.push_back(batch);
  (void)advance(st);

  if (!st.waiting.empty()) {
    arm_timer(st);
    if (waits_for_me && hw::percpu::in_hardirq()) {
      (void)st.dpc.queue();
    }
  }
}

// Takes the batch being filled out of `current`, if there is anything in it.
// @pre Interrupts are off, on the CPU that owns @p st.
[[nodiscard]] RetireBatch* take_current(CpuState& st) noexcept {
  RetireBatch* batch = st.current;
  if (batch == nullptr || batch->empty()) {
    return nullptr;
  }

  st.current = new_batch(st);
  return batch;
}

hw::timer::TimerAction on_timer(hw::timer::Timer&, void* context) noexcept {
  auto& st = *static_cast<CpuState*>(context);
  KASSERT(&st == &local_state());

  // Anything that waited this long for its batch to fill up is sealed as it is.
  if (RetireBatch* batch = take_current(st)) {
    seal(st, batch);
  } else {
    (void)advance(st);
  }

  if (!st.waiting.empty() || (st.current != nullptr && !st.current->empty())) {
    arm_timer(st);
  }

  return hw::timer::TimerAction::Done;
}

// Runs what is on `ready`, one batch at a time, with interrupts on.
// @pre The thread stays on the CPU that owns @p st, and interrupts are on.
std::size_t reclaim_ready(CpuState& st) noexcept {
  std::size_t reclaimed = 0;

  while (true) {
    RetireBatch* batch;
    {
      IrqDisableGuard irq_guard;
      reclaimed += advance(st);
      batch = st.ready.pop_front();
      if (batch == nullptr) {
        return reclaimed;
      }

      // Until this one is done, nothing behind it may be: `completed` counts in order.
      st.reclaiming = true;
    }

    reclaimed += run_entries(*batch);

    IrqDisableGuard irq_guard;
    st.reclaiming = false;
    finish(st, batch);
  }
}

void on_dpc(void* context) { (void)reclaim_ready(*static_cast<CpuState*>(context)); }

// Before initialize() there is the BSP and nothing else, so the only reader there can be is the
// caller: anything else may go at once.
void reclaim_before_initialize(const Entry entry) noexcept {
  if (hw::percpu::in_rcu_read()) [[unlikely]] {
    logger::fatal("rcu: retiring from a read section before rcu::initialize()\n");
  }

  if (entry.fn != nullptr) {
    entry.fn(entry.arg);
  } else {
    memory::heap::kfree(entry.arg);
  }
}

// No memory for a batch to put it in: wait for the readers here, and reclaim it right away.
void reclaim_without_batch(CpuState& st, const Entry entry) noexcept {
  if (hw::percpu::in_rcu_read()) [[unlikely]] {
    // Waiting would be waiting for ourselves. Losing the memory beats freeing it under a reader.
    logger::warn("rcu: no memory to retire {} from a read section; it is leaked\n", entry.arg);
    return;
  }

  wait_for_all_readers();
  if (entry.fn != nullptr) {
    entry.fn(entry.arg);
  } else {
    memory::heap::kfree(entry.arg);
  }

  bump(st.retired);
  bump(st.reclaimed);
}

void enqueue(const Entry entry) noexcept {
  KASSERT(!hw::percpu::in_nmi());

  if (g_states == nullptr) [[unlikely]] {
    reclaim_before_initialize(entry);
    return;
  }

  hw::percpu::preempt_disable();
  CpuState& st = local_state();
  RetireBatch* full = nullptr;
  bool no_batch = false;

  {
    IrqDisableGuard irq_guard;
    if (st.current == nullptr) {
      st.current = new_batch(st);
    }

    if (RetireBatch* batch = st.current; batch != nullptr) [[likely]] {
      batch->entries[batch->count++] = entry;
      batch->callbacks += entry.fn != nullptr ? 1 : 0;
      bump(st.retired);

      if (batch->full()) {
        full = batch;
        st.current = new_batch(st);
      } else if (batch->count == 1) {
        // Something has to seal it if nothing else comes along.
        arm_timer(st);
      }
    } else {
      no_batch = true;
    }
  }

  if (full != nullptr) {
    seal(st, full);
  } else if (no_batch) [[unlikely]] {
    reclaim_without_batch(st, entry);
  }

  hw::percpu::preempt_enable();
}

// Seals the calling CPU's batch, wherever it got to, and moves along what is done. Run on every
// CPU by barrier(), in an IPI.
void flush_here(void* sealed) {
  CpuState& st = local_state();
  if (RetireBatch* batch = take_current(st)) {
    seal(st, batch);
  } else {
    (void)advance(st);
  }

  *static_cast<std::uint64_t*>(sealed) = st.sealed;
}
}  // namespace

void initialize() noexcept {
  if (g_states != nullptr) {
    return;
  }

  const auto cache = memory::heap::KmemCache::create(sizeof(RetireBatch), alignof(RetireBatch));
  if (!cache) {
    logger::fatal("rcu: cannot create the batch cache: {:#x}\n", std::to_underlying(cache.error()));
  }

  g_batch_cache = *cache;

  const std::uint32_t cpus = hw::smp::possible_cpus();
  auto* states =
      static_cast<CpuState*>(memory::heap::kmalloc(sizeof(CpuState) * cpus, alignof(CpuState)));
  if (states == nullptr) {
    logger::fatal("rcu: no memory for the state of {} CPUs\n", cpus);
  }

  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    CpuState& st = *std::construct_at(&states[cpu]);
    st.timer.set_callback(&on_timer, &st);
    std::destroy_at(&st.dpc);
    std::construct_at(&st.dpc, &on_dpc, &st);

    // A batch and a spare for each, so that the first retirements need no allocation.
    st.current = new_batch(st);
    st.spare = new_batch(st);
  }

  g_state_count = cpus;
  g_states = states;
}

void retire_raw(void* ptr) noexcept {
  if (ptr != nullptr) [[likely]] {
    enqueue(Entry{.fn = nullptr, .arg = ptr});
  }
}

void call_after_grace_period(const RetireFn fn, void* arg) noexcept {
  if (fn != nullptr) [[likely]] {
    enqueue(Entry{.fn = fn, .arg = arg});
  }
}

void flush_local_batch() noexcept {
  KASSERT(!hw::percpu::in_nmi());
  if (g_states == nullptr) [[unlikely]] {
    return;
  }

  hw::percpu::preempt_disable();
  CpuState& st = local_state();

  RetireBatch* batch;
  {
    IrqDisableGuard irq_guard;
    batch = take_current(st);
    if (batch == nullptr) {
      (void)advance(st);
    }
  }

  if (batch != nullptr) {
    seal(st, batch);
  }

  hw::percpu::preempt_enable();
}

void synchronize() noexcept {
  KASSERT(!hw::percpu::in_rcu_read());

  wait_for_all_readers();

  if (g_states != nullptr) [[likely]] {
    hw::percpu::preempt_disable();
    bump(local_state().synchronizes);
    hw::percpu::preempt_enable();
  }
}

void barrier() noexcept {
  KASSERT(hw::percpu::can_sleep());
  if (g_states == nullptr) [[unlikely]] {
    return;
  }

  // One CPU at a time: seal what it has, then wait for it to get through all of it. The timer and
  // the DPC of that CPU do the work; this only looks.
  hw::smp::for_each_online([](const std::uint32_t cpu) {
    std::uint64_t target = 0;
    if (!hw::ipi::call(cpu, &flush_here, &target, hw::ipi::Wait::Yes)) [[unlikely]] {
      return;
    }

    const CpuState& st = g_states[cpu];
    while (st.completed.load(std::memory_order_acquire) < target) {
      hw::cpu_relax();
      sched::Scheduler::yield();
    }
  });
}

std::size_t drain_reclaimable() noexcept {
  KASSERT(hw::percpu::can_sleep());
  if (g_states == nullptr) [[unlikely]] {
    return 0;
  }

  hw::percpu::preempt_disable();
  const std::size_t reclaimed = reclaim_ready(local_state());
  hw::percpu::preempt_enable();
  return reclaimed;
}

Stats stats() noexcept {
  Stats total{};
  for (std::uint32_t cpu = 0; cpu < g_state_count; ++cpu) {
    const CpuState& st = g_states[cpu];
    total.objects_retired += st.retired.load(std::memory_order_relaxed);
    total.objects_reclaimed += st.reclaimed.load(std::memory_order_relaxed);
    total.fastpath_batches += st.fastpath_batches.load(std::memory_order_relaxed);
    total.hyaline_batches += st.hyaline_batches.load(std::memory_order_relaxed);
    total.synchronizes += st.synchronizes.load(std::memory_order_relaxed);
  }

  return total;
}
}  // namespace kernel::utils::rcu
