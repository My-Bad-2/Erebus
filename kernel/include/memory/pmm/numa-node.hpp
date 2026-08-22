#pragma once

#include "mobility-zone.hpp"

namespace kernel::memory::pmm {
enum class MemoryTier : std::uint8_t {
  HBM = 0,       // Higher bandwidth memory
  DDR = 1,       // Standard Main Memory
  CXL_LOCAL = 2, // PCIe-attached Compute Express Link Memory
  PMEM = 3,      // Persistent Memory
};

class alignas(std::hardware_destructive_interference_size) NumaNode {
  std::uint32_t m_node_id;

  MemoryTier m_tier;
  std::uint32_t m_bandwidth_mbps;
  std::uint32_t m_latency_ns;

  const PageMobility m_fallback_hierarchy[3][2] = {
      {PageMobility::Reclaimable, PageMobility::Movable},   // Unmovable starved
      {PageMobility::Reclaimable, PageMobility::Unmovable}, // Movable starved
      {PageMobility::Unmovable, PageMobility::Movable},     // Reclaimable starved
  };

  MobilityZone m_zones[3];

public:
  explicit NumaNode(const std::uint32_t id, const MemoryTier tier, const std::uint32_t bandwidth_mbps,
                    const std::uint32_t latency_ns) noexcept
      : m_node_id(id), m_tier(tier), m_bandwidth_mbps(bandwidth_mbps), m_latency_ns(latency_ns),
        m_zones{MobilityZone(this, PageMobility::Unmovable), MobilityZone(this, PageMobility::Movable),
                MobilityZone(this, PageMobility::Reclaimable)} {}

  NumaNode(const NumaNode &) = delete;
  NumaNode &operator=(const NumaNode &) = delete;

  [[nodiscard]] std::uint32_t get_id() const noexcept { return m_node_id; }
  [[nodiscard]] MemoryTier get_tier() const noexcept { return m_tier; }
  [[nodiscard]] std::uint32_t get_bandwidth() const noexcept { return m_bandwidth_mbps; }
  [[nodiscard]] std::uint32_t get_latency() const noexcept { return m_latency_ns; }
  [[nodiscard]] MobilityZone &get_zone(const PageMobility m) noexcept { return m_zones[std::to_underlying(m)]; }

  [[nodiscard]] Page *alloc_pages(PageMobility requested_mobility, std::uint8_t requested_order) noexcept;
};
} // namespace kernel::memory::pmm