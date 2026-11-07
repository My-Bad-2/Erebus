/// @file mobility-zone.hpp
/// @brief One buddy allocator instance: the free memory of one mobility class on one NUMA node.

#pragma once

#include <atomic>
#include <cstdint>

#include "free_area.hpp"
#include "memory/pmm/pcp_cache.hpp"
#include "utils/lock.hpp"
#include "utils/ring_buffer.hpp"

namespace kernel::memory::pmm {
/// @brief Free-page thresholds, in pages, that drive the background daemons.
struct ZoneWatermarks {
  std::uint64_t min;   ///< Emergency reserve.
  std::uint64_t low;   ///< Below this, reclaim is kicked.
  std::uint64_t high;  ///< Reclaim stops once the zone is back above this.
};

class NumaNode;

/// @brief A binary buddy allocator for one (node, mobility) pair.
///
/// Free memory is kept as naturally aligned blocks of 2^order pages in one FreeArea per order.
/// A bitmap of non-empty orders lets allocation find the smallest usable block with a single
/// `tzcnt`. Allocating splits a larger block in halves until it has the requested order, putting
/// the unused halves back; freeing does the reverse and merges a block with its buddy (the block
/// whose PFN differs only in bit `order`) for as long as the buddy is free, has the same order
/// and belongs to this zone.
///
/// Three things sit in front of the locked core:
///  - Per-CPU caches (PcpList) for orders up to PCP_MAX_ORDER. The common alloc/free never
///    takes the lock.
///  - Deferred frees: a CPU on a different NUMA node frees into an MPSC ring instead of
///    bouncing the zone lock across the interconnect. Whoever takes the lock next merges them.
///  - Watermarks that wake the reclaim/compaction daemons (see daemon.hpp).
///
/// Block heads in the free lists are marked PageState::Free. Every other frame of the zone,
/// including the interior frames of free blocks, is PageState::Active; the merge logic relies on
/// that to tell a free buddy from a stale descriptor.
///
/// Pages sitting in per-CPU caches or the deferred ring are not counted in get_free_pages().
class alignas(std::hardware_destructive_interference_size) MobilityZone {
  friend NumaNode;
  friend class Router;

  mutable utils::QSpinlock m_lock;
  std::uint32_t m_active_orders_bitmap{0};
  FreeArea m_areas[MAX_ORDER + 1];

  alignas(
      std::hardware_destructive_interference_size) std::atomic<std::uint64_t> m_total_free_pages{0};

  alignas(std::hardware_destructive_interference_size) PageMobility m_mobility;
  NumaNode* m_parent_node;
  ZoneWatermarks m_watermarks{};
  PcpCache* m_pcp_cache{nullptr};

  alignas(std::hardware_destructive_interference_size)
      utils::MpscRingBuffer<Page*, 512> m_deferred_frees;

  [[nodiscard]] Page* extract_block_internal(std::uint8_t requested_order) noexcept;
  void buddy_merge_internal(Page* page, std::uint8_t order) noexcept;

  void flush_deferred_frees_internal() noexcept;
  void evaluate_watermarks(std::uint64_t curr_free, std::uint8_t requested_order,
                           bool alloc_failed) const noexcept;

  [[nodiscard]] Page* alloc_pages_locked(std::uint8_t requested_order) noexcept;
  void free_page_locked(Page* page, std::uint8_t order) noexcept;

  [[nodiscard]] bool batch_refill_pcp(PcpList& list, std::uint8_t order) noexcept;
  void batch_drain_pcp(PcpList& list, std::uint8_t order) noexcept;

  [[nodiscard]] Page* adopt_stolen_block(Page* block, std::uint8_t block_order,
                                         std::uint8_t requested_order) noexcept;

 public:
  MobilityZone(NumaNode* parent, PageMobility mobility) noexcept
      : m_mobility{mobility}, m_parent_node{parent} {}

  MobilityZone(const MobilityZone&) = delete;
  MobilityZone& operator=(const MobilityZone&) = delete;

  [[nodiscard]] PageMobility get_mobility() const noexcept { return m_mobility; }
  [[nodiscard]] utils::QSpinlock& get_lock() noexcept { return m_lock; }
  [[nodiscard]] ZoneWatermarks get_watermarks() const noexcept { return m_watermarks; }

  [[nodiscard]] std::uint64_t get_free_pages() const noexcept {
    return m_total_free_pages.load(std::memory_order_relaxed);
  }

  [[gnu::always_inline]] void add_free_pages(const std::uint64_t count) noexcept {
    m_total_free_pages.fetch_add(count, std::memory_order_relaxed);
  }

  [[gnu::always_inline]] void sub_free_pages(const std::uint64_t count) noexcept {
    m_total_free_pages.fetch_sub(count, std::memory_order_relaxed);
  }

  [[nodiscard, gnu::always_inline]] bool is_order_active(const std::uint8_t order) const noexcept {
    return (m_active_orders_bitmap & (1u << order)) != 0;
  }

  [[gnu::always_inline]] void set_order_active(const std::uint8_t order) noexcept {
    m_active_orders_bitmap |= (1u << order);
  }

  [[gnu::always_inline]] void clear_order_active(const std::uint8_t order) noexcept {
    m_active_orders_bitmap &= ~(1u << order);
  }

  [[nodiscard, gnu::always_inline]] std::uint32_t get_active_orders_bitmap() const noexcept {
    return m_active_orders_bitmap;
  }

  [[nodiscard, gnu::always_inline]] FreeArea& get_area(const std::uint8_t order) noexcept {
    return m_areas[order];
  }

  [[nodiscard, gnu::always_inline]] const FreeArea& get_area(
      const std::uint8_t order) const noexcept {
    return m_areas[order];
  }

  /// @brief Hands a free block to the zone during boot, without merging.
  /// @pre The block's head descriptor was set up with PageState::Free and this zone's mobility.
  [[gnu::always_inline]] void inject_free_page_cold(Page* page, const std::uint8_t order) noexcept {
    m_areas[order].push_cold(page);
    set_order_active(order);
    add_free_pages(1ul << order);
  }

  /// @brief Derives the watermarks from the zone's size (1%, 3% and 5%).
  void set_watermarks(const std::uint64_t total_present_pages) noexcept {
    m_watermarks.min = total_present_pages / 100;
    m_watermarks.low = (total_present_pages * 3) / 100;
    m_watermarks.high = (total_present_pages * 5) / 100;
  }

  /// @brief Attaches the zone's per-CPU caches (one PcpCache per CPU, zero-initialised).
  void allocate_pcp(void* pcp_memory) noexcept { m_pcp_cache = static_cast<PcpCache*>(pcp_memory); }

  /// @brief Sizes every CPU's cache lists for the zone's current amount of free memory.
  ///
  /// Must be called for every zone that has caches attached, including empty ones: an untuned
  /// list has `batch == 0` and would never pull pages that later migrate into the zone.
  void tune_pcp(const std::uint32_t total_cpus) const noexcept {
    if (!m_pcp_cache) {
      return;
    }

    const std::uint64_t zone_pages = get_free_pages();

    for (std::uint32_t c = 0; c < total_cpus; ++c) {
      for (std::uint8_t order = 0; order <= PCP_MAX_ORDER; ++order) {
        m_pcp_cache[c].lists[order].tune(zone_pages >> order, order);
      }
    }
  }

  /// @brief Allocates a block of 2^@p requested_order pages from this zone only.
  /// @return The head descriptor (PageState::Active, order set), or nullptr if the zone is out of
  ///         blocks of that size.
  [[nodiscard]] Page* alloc_pages(std::uint8_t requested_order) noexcept;

  /// @brief Returns a block to the zone. @p order must be the order it was allocated with.
  void free_page(Page* page, std::uint8_t order) noexcept;

  /// @brief Gives every block the calling CPU keeps for this zone back to it, and merges what was
  ///        left in the deferred ring while at it.
  ///
  /// Those blocks are free, but the zone neither counts them nor can merge them with their
  /// buddies, so this is the cheapest memory reclaim can find, and it helps compaction too.
  /// @pre Interrupts are off: the lists belong to the CPU, which uses them with interrupts off.
  /// @return How many pages came back.
  std::uint64_t drain_pcp_local() noexcept;

  /// @brief Removes the largest free block of at least @p min_order for a sibling zone to adopt.
  /// @return The block, already marked Active so concurrent merges cannot touch it, or nullptr.
  [[nodiscard]] Page* extract_largest_block_for_steal(std::uint8_t min_order) noexcept;

  /// @brief Rough fragmentation estimate: the share of free memory *not* in MAX_ORDER blocks.
  [[nodiscard]] std::uint32_t calculate_fragmentation_index() const noexcept;
};

/// @brief Flipped once the reclaim/compaction threads exist. Until then watermarks are tracked
///        but never acted upon.
inline std::atomic g_daemons_active{false};
}  // namespace kernel::memory::pmm
