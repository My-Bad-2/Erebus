/// @file numa_domain.hpp
/// @brief What the scheduler keeps per NUMA node: who is idle, and who has work to spare.
///
/// One of these lives in every hw::PerNumaNode, and its tables in memory of that node, since the
/// CPUs of the node are the ones hammering them. They answer the two questions that would
/// otherwise mean walking every CPU of the machine:
///
///  - A thread wakes up. Which CPU is idle? The bitmaps say at once, and taking a bit claims the
///    CPU, so two threads waking at the same moment do not both pick it.
///  - A CPU runs out of work. Who has some? A third bitmap has every CPU with a thread it could
///    give away, so an idle CPU goes straight there instead of knocking on every door.
///
/// All of it is hints, read and written without a lock. A bit that is out of date costs at worst
/// a thread a slightly worse CPU or a sleep it could have skipped, and whoever acts on a bit takes
/// the run queue lock and looks at the real state, so nothing here can make a thread get lost.
///
/// A CPU's position in its node is its *slot*. Slots keep the SMT siblings of a core next to each
/// other, so that a thread can go for a core with nobody on it before settling for any idle CPU:
/// sharing a core with something busy is worth a lot less than having one to itself.
#pragma once

#include <atomic>
#include <bit>
#include <bmi_word.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "core/errors.hpp"

namespace kernel::sched {
/// @brief Where a CPU sits. CPUs of one core have the same `core` and must be next to each other.
struct CpuPlacement {
  std::uint32_t cpu;
  std::uint32_t core;
};

/// @brief The scheduler's view of one NUMA node. See the file comment.
class NumaDomain {
  static constexpr std::uint32_t BITS_PER_WORD = 64;

  struct Slot {
    std::uint32_t cpu;
    std::uint32_t core;
  };

  struct Core {
    std::atomic<std::uint32_t> busy;  ///< Siblings that are not idle.
    std::uint32_t first_slot;
    std::uint32_t siblings;
  };

  std::uint32_t m_node_id{0};
  std::uint32_t m_cpu_count{0};
  std::uint32_t m_core_count{0};
  std::uint32_t m_words{0};
  std::uint32_t m_neighbor_count{0};

  Slot* m_slots{nullptr};
  Core* m_cores{nullptr};
  klib::BmiWord* m_idle_cpus{nullptr};   ///< By slot. Set: idle, and up for grabs.
  klib::BmiWord* m_idle_cores{nullptr};  ///< By core. Set: every sibling is idle.
  klib::BmiWord* m_overloaded{nullptr};  ///< By slot. Set: has a thread to spare.
  const std::uint32_t* m_neighbors{nullptr};

  std::size_t m_storage_order{0};
  void* m_storage{nullptr};

  [[nodiscard]] static constexpr std::uint32_t word_of(const std::uint32_t index) noexcept {
    return index / BITS_PER_WORD;
  }

  [[nodiscard]] static constexpr std::uint32_t bit_of(const std::uint32_t index) noexcept {
    return index % BITS_PER_WORD;
  }

  void note_busy(std::uint32_t slot) noexcept;

 public:
  /// @brief Returned when no CPU is available.
  static constexpr std::uint32_t NO_CPU = ~0u;

  constexpr NumaDomain() noexcept = default;

  NumaDomain(const NumaDomain&) = delete;
  NumaDomain& operator=(const NumaDomain&) = delete;

  /// @brief Builds the tables, in memory of node @p node_id. Every CPU starts out busy.
  /// @param cpus      The CPUs of the node. An entry's index is the slot of the CPU.
  /// @param neighbors The other nodes that have CPUs, nearest first.
  /// @return Error::OutOfMemory if the tables do not fit.
  [[nodiscard]] auto initialize(std::uint32_t node_id, std::span<const CpuPlacement> cpus,
                                std::span<const std::uint32_t> neighbors) noexcept
      -> std::expected<void, Error>;

  /// @brief Gives the tables back. Real domains live forever; this is for the tests' own.
  void release() noexcept;

  [[nodiscard]] std::uint32_t node_id() const noexcept { return m_node_id; }
  [[nodiscard]] std::uint32_t cpu_count() const noexcept { return m_cpu_count; }
  [[nodiscard]] std::uint32_t core_count() const noexcept { return m_core_count; }

  /// @brief The CPU in @p slot.
  [[nodiscard]] std::uint32_t cpu_at(const std::uint32_t slot) const noexcept {
    return m_slots[slot].cpu;
  }

  /// @brief The other nodes with CPUs on them, nearest first.
  [[nodiscard]] std::span<const std::uint32_t> neighbors() const noexcept {
    return {m_neighbors, m_neighbor_count};
  }

  /// @brief Publishes the CPU in @p slot as idle. Only that CPU may call this.
  void mark_idle(std::uint32_t slot) noexcept;

  /// @brief Takes the CPU in @p slot off the idle list, if it is still on it.
  void mark_busy(const std::uint32_t slot) noexcept { (void)claim_cpu(slot); }

  /// @brief Claims the CPU in @p slot.
  /// @return false if it is not idle, or someone else got there first.
  [[nodiscard]] bool claim_cpu(std::uint32_t slot) noexcept;

  /// @brief Claims a CPU of a core that has nothing running on it at all.
  /// @return The CPU, or NO_CPU if there is no such core.
  [[nodiscard]] std::uint32_t claim_idle_core() noexcept;

  /// @brief Claims any idle CPU.
  /// @return The CPU, or NO_CPU if all are busy.
  [[nodiscard]] std::uint32_t claim_idle_cpu() noexcept;

  /// @brief How many CPUs are idle right now.
  [[nodiscard]] std::uint32_t idle_cpu_count() const noexcept;

  /// @brief Says whether the CPU in @p slot has threads another CPU could take.
  void set_overloaded(std::uint32_t slot, bool overloaded) noexcept;

  /// @brief Calls `fn(cpu)` for every overloaded CPU until it returns true, beginning at the word
  ///        of @p start_slot so that the CPUs of a node do not all go for the same victim.
  template <typename Fn>
  void for_each_overloaded(const std::uint32_t start_slot, Fn&& fn) const noexcept {
    for (std::uint32_t step = 0; step < m_words; ++step) {
      const std::uint32_t word = (word_of(start_slot) + step) % m_words;

      for (std::uint64_t bits = m_overloaded[word].snapshot(); bits != 0; bits &= bits - 1) {
        const auto bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        if (fn(cpu_at(word * BITS_PER_WORD + bit))) {
          return;
        }
      }
    }
  }
};
}  // namespace kernel::sched
