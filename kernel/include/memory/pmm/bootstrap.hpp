#pragma once

#include "memory/address.hpp"
#include <span>

#include "numa-node.hpp"

namespace kernel::memory::pmm {
using EarlyAllocFn = VirtualAddress (*)(std::size_t count, std::size_t alignment);

struct MemoryDomain {
  PhysicalAddress base;
  PhysicalAddress end;
  std::uint32_t node_id; // dense internal ID [0, N - 1]
};

struct NodeMetrics {
  MemoryTier tier{MemoryTier::DDR};
  std::uint32_t bandwidth_mbps{64000};
  std::uint32_t base_latency_ns{80};
};

class TopologyParser {
  std::size_t m_domain_count{0};
  MemoryDomain *m_domains{nullptr};

  std::uint32_t m_active_nodes{0};
  std::uint32_t *m_acpi_to_internal_map{nullptr};

  std::uint32_t *m_latencies{nullptr};
  NodeMetrics *m_node_metrics{nullptr};

  // Converts a sparse ACPI proximity domain to a dense internal Node ID
  [[nodiscard]] std::uint32_t get_internal_node_id(const std::uint32_t acpi_domain) const noexcept {
    if (!m_acpi_to_internal_map) {
      return 0;
    }

    auto span = std::span{m_acpi_to_internal_map, m_active_nodes};
    auto it = std::ranges::lower_bound(span, acpi_domain);
    if (it != span.end() && *it == acpi_domain) {
      return std::distance(span.begin(), it);
    }

    return 0xFFFFFFFF; // Not found / No memory attached
  }

  void parse_srat(EarlyAllocFn alloc) noexcept;
  void parse_slit(EarlyAllocFn alloc) noexcept;
  void process_hmat_locality(const void *slb) const noexcept;
  void parse_hmat() const noexcept;

public:
  TopologyParser() = default;

  [[nodiscard]] std::uint32_t active_nodes() const noexcept { return m_active_nodes; }
  [[nodiscard]] std::size_t domain_count() const noexcept { return m_domain_count; }
  [[nodiscard]] std::span<const MemoryDomain> domains() const noexcept { return {m_domains, m_domain_count}; }

  [[nodiscard]] std::uint32_t get_latency(const std::uint32_t src, const std::uint32_t tgt) const noexcept {
    if (!m_latencies || src >= m_active_nodes || tgt >= m_active_nodes) {
      return (src == tgt) ? 10 : 20; // Default UMA/NUMA heuristics
    }

    return m_latencies[src * m_active_nodes + tgt];
  }

  [[nodiscard]] const NodeMetrics &get_node_metrics(const std::uint32_t node_id) const noexcept {
    return m_node_metrics[node_id];
  }

  void parse(EarlyAllocFn &&alloc) noexcept;
};

inline TopologyParser g_topology;
} // namespace kernel::memory::pmm