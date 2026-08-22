#pragma once

#include <atomic>
#include <cstdint>

#include "free_area.hpp"
#include "memory/pmm/pcp_cache.hpp"
#include "utils/lock.hpp"
#include "utils/ring_buffer.hpp"

namespace kernel::memory::pmm {
struct ZoneWatermarks {
  std::uint64_t min;  // absolute min. reserve (OOM)
  std::uint64_t low;  // trigger background reclaim/compaction
  std::uint64_t high; // stop background reclaim/compaction
};

class NumaNode;

std::uint64_t page_to_pfn(const Page *page) noexcept;
Page *pfn_to_page(std::uint64_t pfn) noexcept;

class alignas(std::hardware_destructive_interference_size) MobilityZone {
  friend NumaNode;

  utils::QSpinlock m_lock;
  std::uint32_t m_active_orders_bitmap{0};
  std::atomic<std::uint64_t> m_total_free_pages;

  PageMobility m_mobility;
  NumaNode *m_parent_node;
  ZoneWatermarks m_watermarks{};
  PcpCache *m_pcp_cache{nullptr};

  utils::MpscRingBuffer<Page *, 512> m_deferred_frees;
  FreeArea m_areas[MAX_ORDER + 1];

  void buddy_merge_internal(Page *page, std::uint8_t order) noexcept;
  void flush_deferred_frees_internal() noexcept;

  [[nodiscard]] Page *alloc_pages_locked(std::uint8_t requested_order) noexcept;
  void free_page_locked(Page *page, std::uint8_t order) noexcept;

  // PCP Batch Engines
  bool scavenge_deferred_frees(PcpList &list) noexcept;
  bool batch_refill_pcp(PcpList &list, std::uint8_t order) noexcept;
  void batch_drain_pcp(PcpList &list, std::uint8_t order) noexcept;

public:
  MobilityZone(NumaNode *parent, PageMobility mobility) noexcept : m_mobility{mobility}, m_parent_node{parent} {}

  MobilityZone(const MobilityZone &) = delete;
  MobilityZone &operator=(const MobilityZone &) = delete;

  [[nodiscard]] PageMobility get_mobility() const noexcept { return m_mobility; }
  [[nodiscard]] utils::QSpinlock &get_lock() noexcept { return m_lock; }
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

  [[nodiscard, gnu::always_inline]] FreeArea &get_area(const std::uint8_t order) noexcept { return m_areas[order]; }

  [[nodiscard, gnu::always_inline]] const FreeArea &get_area(const std::uint8_t order) const noexcept {
    return m_areas[order];
  }

  [[gnu::always_inline]] void inject_free_page_cold(Page *page, const std::uint8_t order) noexcept {
    m_areas[order].push_cold(page);
    set_order_active(order);
    add_free_pages(1ul << order);
  }

  void set_watermarks(const std::uint64_t total_present_pages) noexcept {
    m_watermarks.min = total_present_pages / 100;        // 1%
    m_watermarks.low = (total_present_pages * 3) / 100;  // 3%
    m_watermarks.high = (total_present_pages * 5) / 100; // 5%
  }

  void allocate_pcp(void *pcp_memory) noexcept { m_pcp_cache = static_cast<PcpCache *>(pcp_memory); }

  void tune_pcp(const std::uint32_t total_cpus) noexcept {
    if (!m_pcp_cache) {
      return;
    }

    const std::uint64_t zone_pages = get_free_pages();

    for (std::uint32_t c = 0; c < total_cpus; ++c) {
      for (std::uint8_t order = 0; order <= PCP_MAX_ORDER; ++order) {
        m_pcp_cache[c].lists[order].tune(zone_pages >> order);
      }
    }
  }

  [[nodiscard]] Page *alloc_pages(std::uint8_t requested_order) noexcept;
  void free_page(Page *page, std::uint8_t order) noexcept;

  [[nodiscard]] Page *extract_largest_block_for_steal(std::uint8_t min_order) noexcept;
  [[nodiscard]] std::uint32_t calculate_fragmentation_index() const noexcept;
};

inline std::atomic<bool> g_daemons_active{false};
} // namespace kernel::memory::pmm