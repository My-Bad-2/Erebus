/// @file bootstrap.hpp
/// @brief Discovery of the NUMA memory topology from ACPI (SRAT, SLIT and HMAT).

#pragma once

#include <algorithm>
#include <span>

#include "memory/address.hpp"
#include "numa-node.hpp"

namespace kernel::memory::pmm {
/// @brief Allocator used before the heap exists. Returns zeroed, permanently allocated memory.
using EarlyAllocFn = VirtualAddress (*)(std::size_t count, std::size_t alignment);

/// @brief A physical range `[base, end)` owned by one NUMA node.
struct MemoryDomain {
  PhysicalAddress base;
  PhysicalAddress end;
  std::uint32_t node_id;  ///< Dense internal ID in `[0, active_nodes)`.
};

/// @brief Performance characteristics of a node, with sane DDR defaults when HMAT is missing.
struct NodeMetrics {
  MemoryTier tier{MemoryTier::DDR};
  std::uint32_t bandwidth_mbps{64000};
  std::uint32_t base_latency_ns{80};
};

/// @brief Parses the firmware's NUMA description into something the PMM can use directly.
///
/// ACPI identifies nodes by "proximity domain", which can be any 32-bit number. The parser
/// collects the domains that actually have memory, sorts them and renumbers them densely, so
/// that the rest of the PMM can index plain arrays by node ID. Machines without an SRAT are
/// treated as a single node 0 covering everything.
class TopologyParser {
  std::size_t m_domain_count{0};
  MemoryDomain* m_domains{nullptr};

  std::uint32_t m_active_nodes{0};
  std::uint32_t* m_acpi_to_internal_map{nullptr};

  std::uint32_t* m_latencies{nullptr};
  NodeMetrics* m_node_metrics{nullptr};

  void parse_srat(EarlyAllocFn alloc) noexcept;
  void parse_slit(EarlyAllocFn alloc) noexcept;
  void process_hmat_locality(const void* slb) const noexcept;
  void parse_hmat() const noexcept;

 public:
  static constexpr std::uint32_t INVALID_NODE = 0xFFFFFFFF;

  TopologyParser() = default;

  [[nodiscard]] std::uint32_t active_nodes() const noexcept { return m_active_nodes; }

  /// @brief Converts a sparse ACPI proximity domain into a dense internal node ID.
  /// @return INVALID_NODE if no memory is attached to that domain.
  [[nodiscard]] std::uint32_t get_internal_node_id(const std::uint32_t acpi_domain) const noexcept {
    if (!m_acpi_to_internal_map) {
      return 0;
    }

    const auto span = std::span{m_acpi_to_internal_map, m_active_nodes};
    const auto it = std::ranges::lower_bound(span, acpi_domain);
    if (it != span.end() && *it == acpi_domain) {
      return static_cast<std::uint32_t>(std::distance(span.begin(), it));
    }

    return INVALID_NODE;
  }

  [[nodiscard]] std::size_t domain_count() const noexcept { return m_domain_count; }

  /// @brief Memory domains sorted by base address, guaranteed not to overlap.
  [[nodiscard]] std::span<const MemoryDomain> domains() const noexcept {
    return {m_domains, m_domain_count};
  }

  /// @brief SLIT distance between two nodes (10 means local). Defaults to 10/20 without SLIT.
  [[nodiscard]] std::uint32_t get_latency(const std::uint32_t src,
                                          const std::uint32_t tgt) const noexcept {
    if (!m_latencies || src >= m_active_nodes || tgt >= m_active_nodes) {
      return (src == tgt) ? 10 : 20;
    }

    return m_latencies[src * m_active_nodes + tgt];
  }

  [[nodiscard]] const NodeMetrics& get_node_metrics(const std::uint32_t node_id) const noexcept {
    return m_node_metrics[node_id];
  }

  /// @brief Reads SRAT, SLIT and HMAT. Everything is allocated with @p alloc and kept forever.
  void parse(EarlyAllocFn alloc) noexcept;
};

inline TopologyParser g_topology;
}  // namespace kernel::memory::pmm
