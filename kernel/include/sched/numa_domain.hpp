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
///
/// The same goes for everything else CPUs can share, which is what turns a group of them into a
/// range of slots (a SlotSpan), and "something nearby" into a look at part of a bitmap. See
/// balance.hpp for why.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <bmi_word.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "core/errors.hpp"

namespace kernel::sched {
/// @brief What a group of CPUs has in common, from the closest to the furthest apart.
enum class Sharing : std::uint8_t {
  Core,     ///< One core: SMT siblings.
  Cluster,  ///< A mid-level cache, or a module.
  Cache,    ///< The last-level cache.
  Package,  ///< The same chip.
};

inline constexpr std::uint32_t SHARING_LEVELS = 4;

/// @brief Where a CPU sits. CPUs with a group in common have to be next to each other, which comes
///        down to being sorted by these, from the back.
struct CpuPlacement {
  std::uint32_t cpu;
  std::uint32_t core;
  std::uint32_t cluster{0};
  std::uint32_t cache{0};
  std::uint32_t package{0};
};

/// @brief A group of CPUs, as slots of their node.
struct SlotSpan {
  std::uint32_t first{0};
  std::uint32_t count{0};

  [[nodiscard]] constexpr bool contains(const std::uint32_t slot) const noexcept {
    return slot - first < count;
  }

  [[nodiscard]] constexpr bool operator==(const SlotSpan&) const noexcept = default;
};

/// @brief The scheduler's view of one NUMA node. See the file comment.
class NumaDomain {
  static constexpr std::uint32_t BITS_PER_WORD = 64;

  struct Slot {
    std::uint32_t cpu;
    std::uint32_t core;
    std::uint32_t capacity;
    std::array<SlotSpan, SHARING_LEVELS> groups;  ///< By Sharing.
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
  std::uint32_t m_top_capacity{0};
  bool m_mixed_capacity{false};  ///< Not all of them are the same. Most machines can skip a step.

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

  void note_busy(std::uint32_t slot) const noexcept;

  [[nodiscard]] std::uint32_t claim_idle_core_in(SlotSpan span, bool fast_only) noexcept;
  [[nodiscard]] std::uint32_t claim_idle_cpu_in(SlotSpan span, bool fast_only) noexcept;

  [[nodiscard]] SlotSpan whole_node() const noexcept { return {0, m_cpu_count}; }

  [[nodiscard]] bool is_fast(const std::uint32_t slot) const noexcept {
    return m_slots[slot].capacity == m_top_capacity;
  }

  // Calls `fn(index)` for the bits set in a range of @p bitmap until it returns true, which it then
  // returns as well.
  template <typename Fn>
  static bool for_each_bit(const klib::BmiWord* bitmap, const std::uint32_t first,
                           const std::uint32_t end, Fn&& fn) noexcept {
    for (std::uint32_t word = word_of(first); word * BITS_PER_WORD < end; ++word) {
      const std::uint32_t base = word * BITS_PER_WORD;
      std::uint64_t bits = bitmap[word].snapshot();

      if (first > base) {
        bits &= ~std::uint64_t{0} << (first - base);
      }

      if (end - base < BITS_PER_WORD) {
        bits &= (std::uint64_t{1} << (end - base)) - 1;
      }

      for (; bits != 0; bits &= bits - 1) {
        if (fn(base + static_cast<std::uint32_t>(std::countr_zero(bits)))) {
          return true;
        }
      }
    }

    return false;
  }

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

  /// @brief The group that the CPU in @p slot is in at @p level.
  [[nodiscard]] SlotSpan group_of(const std::uint32_t slot, const Sharing level) const noexcept {
    return m_slots[slot].groups[static_cast<std::uint32_t>(level)];
  }

  /// @brief How much the CPU in @p slot gets done, see balance.hpp.
  [[nodiscard]] std::uint32_t capacity_of(const std::uint32_t slot) const noexcept {
    return m_slots[slot].capacity;
  }

  /// @brief Says how much the CPU in @p slot gets done.
  void set_capacity(std::uint32_t slot, std::uint32_t capacity) noexcept;

  /// @brief The other nodes with CPUs on them, nearest first.
  [[nodiscard]] std::span<const std::uint32_t> neighbors() const noexcept {
    return {m_neighbors, m_neighbor_count};
  }

  /// @brief Publishes the CPU in @p slot as idle. Only that CPU may call this.
  void mark_idle(std::uint32_t slot) const noexcept;

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

  /// @brief Claims an idle CPU that shares the last-level cache with the one in @p slot, a whole
  ///        core if there is one.
  /// @return The CPU, or NO_CPU if there is none.
  [[nodiscard]] std::uint32_t claim_near(std::uint32_t slot) noexcept;

  /// @brief How many CPUs are idle right now.
  [[nodiscard]] std::uint32_t idle_cpu_count() const noexcept;

  /// @brief Says whether the CPU in @p slot has threads another CPU could take.
  void set_overloaded(std::uint32_t slot, bool overloaded) const noexcept;

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

  /// @brief Calls `fn(cpu)` for every overloaded CPU until it returns true, in order of what they
  ///        share with the one in @p slot: first its core, then ever wider groups out to the node.
  ///
  /// Within a group this goes around from @p slot, so that the idle CPUs of one group do not all
  /// land on the same victim.
  template <typename Fn>
  void for_each_overloaded_near(const std::uint32_t slot, Fn&& fn) const noexcept {
    SlotSpan seen{.first = slot, .count = 1};

    const auto visit = [&](const std::uint32_t candidate) {
      return !seen.contains(candidate) && fn(cpu_at(candidate));
    };

    const auto look_in = [&](const SlotSpan span) {
      const std::uint32_t end = span.first + span.count;
      return for_each_bit(m_overloaded, slot, end, visit) ||
             for_each_bit(m_overloaded, span.first, slot, visit);
    };

    for (const SlotSpan group : m_slots[slot].groups) {
      if (group != seen) {
        if (look_in(group)) {
          return;
        }

        seen = group;
      }
    }

    (void)look_in(whole_node());
  }
};
}  // namespace kernel::sched
