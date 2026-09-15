#pragma once

#include "bootstrap.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "numa-node.hpp"

namespace kernel::memory::pmm {
class Router {
public:
  static constexpr std::uint32_t MAX_ACCEPTABLE_LATENCY_NS = 800;

  struct FallbackRoute {
    std::uint32_t target_node;
    MemoryTier tier;
    std::uint32_t latency_ns;
    std::uint32_t bandwidth_mbps;
  };

private:
  struct RoutingTableEntry {
    NumaNode *node;
    FallbackRoute *fallbacks;
    std::uint32_t fallback_count;
  };

  std::uint32_t m_num_active_nodes{0};
  RoutingTableEntry *m_routing_table{nullptr};
  FallbackRoute *m_fallback_matrix{nullptr};
  mutable std::atomic<std::uint64_t> *m_starved_nodes{nullptr};

  [[nodiscard, gnu::always_inline]] bool is_node_starved(const std::uint32_t node_id) const noexcept {
    const std::uint64_t mask = m_starved_nodes[node_id / 64].load(std::memory_order_relaxed);
    return (mask & (1ul << (node_id % 64))) != 0;
  }

  [[gnu::always_inline]] void mark_node_starved(const std::uint32_t node_id) const noexcept {
    m_starved_nodes[node_id / 64].fetch_or(1ul << (node_id % 64), std::memory_order_relaxed);
  }

  [[gnu::always_inline]] void clear_node_starved(const std::uint32_t node_id) const noexcept {
    m_starved_nodes[node_id / 64].fetch_and(~(1ul << (node_id % 64)), std::memory_order_release);
  }

public:
  Router() = default;

  void initialize(std::uint32_t active_nodes, NumaNode **nodes_array, const std::uint32_t *slit_latencies,
                  EarlyAllocFn alloc) noexcept;

  [[nodiscard]] std::uint32_t get_active_nodes() const noexcept { return m_num_active_nodes; }
  [[nodiscard]] NumaNode &get_node(const std::uint32_t node_id) const noexcept {
    return *m_routing_table[node_id].node;
  }

  [[nodiscard]] Page *alloc_single(std::uint32_t preferred_node, PageMobility mobility,
                                   std::uint8_t order) const noexcept;

  std::size_t alloc_bulk(std::uint32_t preferred_node, PageMobility mobility, std::uint8_t order,
                         std::span<Page *> out_span) const noexcept;

  void free_single(Page *page, std::uint8_t order) const noexcept;
};

inline Router g_router;
} // namespace kernel::memory::pmm