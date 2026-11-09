/// @file srcu.hpp
/// @brief Sleepable RCU: read sections that may block, in a domain of their own.
///
/// @code
/// constinit rcu::SleepableDomain g_tables{};  // g_tables.initialize() once the heap works
///
/// {
///   rcu::SleepableReadGuard guard{g_tables};
///   const Table* table = g_table.load(guard);
///   lookup_that_may_fault_or_take_a_mutex(table);
/// }
///
/// Table* old = g_table.exchange(fresh);
/// g_tables.synchronize();
/// delete old;
/// @endcode
///
/// A ReadGuard (rcu.hpp) cannot sleep, which is what lets its writers wait without a care. Some
/// readers have to: a walk that may fault in a page, or take a Mutex. Those get a domain, and only
/// writers of that domain ever wait for them, so a reader that sleeps for a second holds up no one
/// else.
///
/// ### Working
///
/// The domain has two sets of per-CPU counters, one per generation, each counting the read sections
/// that started (`locks`) and ended (`unlocks`) in it. A reader adds to the `locks` of the current
/// generation on whatever CPU it is on, and to the `unlocks` of the same generation on whatever CPU
/// it ends up on. Readers can move between CPUs while they sleep, so no single CPU's counters mean
/// anything, only the sums over all of them.
///
/// synchronize() waits for the generation that is not current to drain, from readers that read the
/// generation just before the last flip, flips, and waits for the old current one to drain as well.
/// A generation is drained when its locks and unlocks add up to the same number, and the order the
/// sums are taken in matters: all the unlocks first, then a full fence, then the locks. A reader
/// whose unlock was counted then had its lock counted too, while the other order could count the
/// unlock of one reader against the lock of another that is still inside.
///
/// Both sides use locked increments, which are full barriers: a reader's loads cannot start before
/// its lock is counted, nor go on after its unlock is.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "sched/mutex.hpp"

namespace kernel::utils::rcu {
/// @brief A sleepable RCU domain. See the file comment.
///
/// Usable before initialize(): until then every CPU counts in one shared pair of counters, which
/// is correct, only slower.
class alignas(std::hardware_destructive_interference_size) SleepableDomain {
 public:
  constexpr SleepableDomain() noexcept = default;

  SleepableDomain(const SleepableDomain&) = delete;
  SleepableDomain& operator=(const SleepableDomain&) = delete;

  /// @brief Gives every CPU counters of its own.
  /// @return false without memory, in which case the domain goes on with the shared ones.
  [[nodiscard]] bool initialize() noexcept;

  /// @brief Gives the per-CPU counters back.
  /// @pre Nobody is in a read section of the domain, or in synchronize(), and nobody will be.
  void destroy() noexcept;

  /// @brief Enters a read section. @return The token read_unlock() wants back.
  [[nodiscard, gnu::always_inline]] std::uint32_t read_lock() noexcept {
    const std::uint32_t generation = m_generation.load(std::memory_order_relaxed) & 1u;
    counters().locks[generation].fetch_add(1, std::memory_order_seq_cst);
    return generation;
  }

  /// @brief Leaves the read section that read_lock() returned @p token for, on whatever CPU.
  [[gnu::always_inline]] void read_unlock(const std::uint32_t token) noexcept {
    counters().unlocks[token & 1u].fetch_add(1, std::memory_order_seq_cst);
  }

  /// @brief Waits until every read section of this domain that started before the call is over,
  ///        sleeping meanwhile. Calls from several threads take turns.
  /// @pre The calling thread can sleep, and is not in a read section of this domain.
  void synchronize() noexcept;

  /// @brief Whether a read section of the domain started in generation @p generation and has not
  ///        ended. What synchronize() waits on. Anywhere else it is good for assertions at most,
  ///        as the answer can change at once.
  [[nodiscard]] bool has_readers(std::uint32_t generation) const noexcept;

 private:
  struct alignas(std::hardware_destructive_interference_size) Counters {
    std::array<std::atomic<std::uint64_t>, 2> locks{};
    std::array<std::atomic<std::uint64_t>, 2> unlocks{};
  };

  // Any CPU's counters will do for correctness. The calling CPU's keep the cache line local. A
  // thread that moves between the two loads only counts on another CPU's. CPU ids are below
  // possible_cpus(), which is what the array was sized by.
  [[nodiscard, gnu::always_inline]] Counters& counters() noexcept {
    Counters* per_cpu = m_per_cpu.load(std::memory_order_acquire);
    return per_cpu != nullptr ? per_cpu[hw::percpu::id()] : m_shared;
  }

  void wait_until_drained(std::uint32_t generation) const noexcept;

  std::atomic<Counters*> m_per_cpu{nullptr};
  std::uint32_t m_cpu_count{0};  ///< Entries in m_per_cpu.
  std::atomic<std::uint32_t> m_generation{0};
  sched::Mutex m_sync{};
  Counters m_shared{};  ///< Before initialize(), and for readers that started before it.
};

/// @brief A read section of a SleepableDomain. The thread may sleep, block and move to other CPUs
///        while it holds one.
class [[nodiscard]] SleepableReadGuard {
  SleepableDomain& m_domain;
  std::uint32_t m_token;

 public:
  explicit SleepableReadGuard(SleepableDomain& domain) noexcept
      : m_domain(domain), m_token(domain.read_lock()) {}

  ~SleepableReadGuard() noexcept { m_domain.read_unlock(m_token); }

  SleepableReadGuard(const SleepableReadGuard&) = delete;
  SleepableReadGuard& operator=(const SleepableReadGuard&) = delete;
  SleepableReadGuard(SleepableReadGuard&&) = delete;
  SleepableReadGuard& operator=(SleepableReadGuard&&) = delete;

  /// What makes it count as a read guard for rcu::Ptr::load().
  static constexpr void is_rcu_read_guard_witness() noexcept {}
};
}  // namespace kernel::utils::rcu
