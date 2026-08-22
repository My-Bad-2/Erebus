#pragma once

#include "memory/address.hpp"
#include <span>

#include "drivers/acpi.hpp"
#include "numa-node.hpp"

namespace kernel::memory::pmm {
struct MemoryDomain {
  PhysicalAddress base;
  PhysicalAddress end;
  std::uint32_t node_id;
};

struct NodeMetrics {
  MemoryTier tier{MemoryTier::DDR};
  std::uint32_t bandwidth_mbps{64000};
  std::uint32_t base_latency_ns{80};
};

using EarlyAllocFn = VirtualAddress (*)(std::size_t count, std::size_t alignment);

class TopologyParser {
  std::size_t m_domain_count{0};
  MemoryDomain *m_domains{nullptr};

  std::uint32_t m_active_nodes{1};
  std::uint32_t *m_latencies{nullptr};
  NodeMetrics *m_node_metrics{nullptr};

  void parse_srat(EarlyAllocFn alloc) noexcept;
  void parse_slit(EarlyAllocFn alloc) noexcept;
  void process_hmat_locality(const void *sllb) const noexcept;
  void parse_hmat() noexcept;

public:
  TopologyParser() = default;

  [[nodiscard]] std::uint32_t active_nodes() const noexcept { return m_active_nodes; }
  [[nodiscard]] std::size_t domain_count() const noexcept { return m_domain_count; }
  [[nodiscard]] std::span<const MemoryDomain> domains() const noexcept { return {m_domains, m_domain_count}; }

  [[nodiscard]] std::uint32_t get_latency(const std::uint32_t src, const std::uint32_t tgt) const noexcept {
    if (!m_latencies || src >= m_active_nodes || tgt >= m_active_nodes) {
      return (src == tgt) ? 10 : 20;
    }

    return m_latencies[src * m_active_nodes + tgt];
  }

  [[nodiscard]] const NodeMetrics &get_node_metrics(const std::uint32_t node_id) const noexcept {
    return m_node_metrics[node_id];
  }

  void parse(EarlyAllocFn &&alloc) noexcept;
};
} // namespace kernel::memory::pmm