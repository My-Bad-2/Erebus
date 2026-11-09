#include "utils/rcu/srcu.hpp"

#include <algorithm>
#include <chrono.hpp>
#include <memory>

#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "sched/sleep.hpp"
#include "utils/assert.hpp"

namespace kernel::utils::rcu {
namespace {
// How long a writer sleeps between looks at a generation that has not drained, at first and at
// most. Readers that do not sleep are usually gone after the first; those that do are worth
// sleeping for longer.
constexpr klib::Duration FIRST_BACKOFF = klib::Duration::microseconds(20);
constexpr klib::Duration MAX_BACKOFF = klib::Duration::milliseconds(10);
}  // namespace

bool SleepableDomain::initialize() noexcept {
  if (m_per_cpu.load(std::memory_order_acquire) != nullptr) {
    return true;
  }

  const std::uint32_t cpus = hw::smp::possible_cpus();
  auto* per_cpu =
      static_cast<Counters*>(memory::heap::kmalloc(sizeof(Counters) * cpus, alignof(Counters)));
  if (per_cpu == nullptr) {
    return false;
  }

  std::uninitialized_default_construct_n(per_cpu, cpus);
  m_cpu_count = cpus;
  m_per_cpu.store(per_cpu, std::memory_order_release);
  return true;
}

void SleepableDomain::destroy() noexcept {
  memory::heap::kfree(m_per_cpu.exchange(nullptr, std::memory_order_acq_rel));
  m_cpu_count = 0;
}

bool SleepableDomain::has_readers(const std::uint32_t generation) const noexcept {
  const std::uint32_t g = generation & 1u;
  const Counters* per_cpu = m_per_cpu.load(std::memory_order_acquire);
  const std::uint32_t cpus = per_cpu != nullptr ? m_cpu_count : 0;

  // Every unlock before any lock, with a full fence in between. See the file comment.
  std::uint64_t unlocks = m_shared.unlocks[g].load(std::memory_order_acquire);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    unlocks += per_cpu[cpu].unlocks[g].load(std::memory_order_acquire);
  }

  std::atomic_thread_fence(std::memory_order_seq_cst);

  std::uint64_t locks = m_shared.locks[g].load(std::memory_order_acquire);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    locks += per_cpu[cpu].locks[g].load(std::memory_order_acquire);
  }

  return locks != unlocks;
}

void SleepableDomain::wait_until_drained(const std::uint32_t generation) const noexcept {
  klib::Duration backoff = FIRST_BACKOFF;
  while (has_readers(generation)) {
    (void)sched::sleep_for(backoff);
    backoff = std::min(backoff * 2, MAX_BACKOFF);
  }
}

void SleepableDomain::synchronize() noexcept {
  KASSERT(hw::percpu::can_sleep());
  m_sync.lock();

  // Whatever the caller unlinked is visible to every reader that starts from here on.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // Readers that read the generation just before the last flip may have counted themselves in
  // after it, in the one that is not current. They go first.
  const std::uint32_t current = m_generation.load(std::memory_order_relaxed) & 1u;
  wait_until_drained(current ^ 1u);

  m_generation.store(current ^ 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // New readers count in the other one now, so this one only drains.
  wait_until_drained(current);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  m_sync.unlock();
}
}  // namespace kernel::utils::rcu
