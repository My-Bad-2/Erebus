/// @file numa-node.hpp
/// @brief A NUMA memory node and its three mobility zones.

#pragma once

#include <array>
#include <utility>

#include "mobility-zone.hpp"

namespace kernel::memory::pmm {
/// @brief Rough classification of a node's memory technology, derived from HMAT when available.
enum class MemoryTier : std::uint8_t {
  HBM = 0,        // High bandwidth memory
  DDR = 1,        // Standard main memory
  CXL_LOCAL = 2,  // PCIe-attached Compute Express Link memory
  PMEM = 3,       // Persistent memory
};

/// @brief All physical memory attached to one proximity domain.
///
/// Memory is split into one MobilityZone per PageMobility class. A request is first served by the
/// zone matching its mobility; if that zone is empty the node steals a block from a sibling zone
/// (in the order given by `s_fallback_hierarchy`) and keeps the remainder in the requesting
/// zone, so the next requests of that class find memory without stealing again.
class alignas(std::hardware_destructive_interference_size) NumaNode {
  std::uint32_t m_node_id;
  MemoryTier m_tier;
  std::uint32_t m_bandwidth_mbps;
  std::uint32_t m_latency_ns;

  static constexpr std::array<std::array<PageMobility, 2>, 3> s_fallback_hierarchy = {{
      {PageMobility::Reclaimable, PageMobility::Movable},    // Unmovable starved
      {PageMobility::Reclaimable, PageMobility::Unmovable},  // Movable starved
      {PageMobility::Unmovable, PageMobility::Movable},      // Reclaimable starved
  }};

  MobilityZone m_zones[3];

  // Pages handed to the zones at boot. Written only then, by the BSP, before anyone reads it.
  std::uint64_t m_managed_pages{0};

 public:
  explicit NumaNode(const std::uint32_t id, const MemoryTier tier,
                    const std::uint32_t bandwidth_mbps, const std::uint32_t latency_ns) noexcept
      : m_node_id(id),
        m_tier(tier),
        m_bandwidth_mbps(bandwidth_mbps),
        m_latency_ns(latency_ns),
        m_zones{MobilityZone(this, PageMobility::Unmovable),
                MobilityZone(this, PageMobility::Movable),
                MobilityZone(this, PageMobility::Reclaimable)} {}

  NumaNode(const NumaNode&) = delete;
  NumaNode& operator=(const NumaNode&) = delete;

  [[nodiscard]] std::uint32_t get_id() const noexcept { return m_node_id; }
  [[nodiscard]] MemoryTier get_tier() const noexcept { return m_tier; }
  [[nodiscard]] std::uint32_t get_bandwidth() const noexcept { return m_bandwidth_mbps; }
  [[nodiscard]] std::uint32_t get_latency() const noexcept { return m_latency_ns; }
  [[nodiscard]] MobilityZone& get_zone(const PageMobility m) noexcept {
    return m_zones[std::to_underlying(m)];
  }

  [[nodiscard]] const MobilityZone& get_zone(const PageMobility m) const noexcept {
    return m_zones[std::to_underlying(m)];
  }

  /// @brief The RAM of the node the PMM manages, free or not. Fixed once the PMM is up.
  [[nodiscard]] std::uint64_t managed_pages() const noexcept { return m_managed_pages; }

  /// @brief Counts @p pages more as the node's. Only while the PMM hands RAM to the zones.
  void add_managed_pages(const std::uint64_t pages) noexcept { m_managed_pages += pages; }

  /// @brief What the node's zones have free. See pmm::MemoryInfo for what that leaves out.
  [[nodiscard]] std::uint64_t free_pages() const noexcept {
    std::uint64_t free = 0;
    for (const MobilityZone& zone : m_zones) {
      free += zone.get_free_pages();
    }

    return free;
  }

  /// @brief Allocates from this node only, stealing from sibling zones if necessary.
  /// @return The block's head descriptor, or nullptr if no zone of the node can satisfy it.
  [[nodiscard]] Page* alloc_pages(PageMobility requested_mobility,
                                  std::uint8_t requested_order) noexcept;
};
}  // namespace kernel::memory::pmm
