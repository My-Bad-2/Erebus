/// @file router.hpp
/// @brief NUMA-aware placement of page allocations across nodes.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "bootstrap.hpp"
#include "numa-node.hpp"

namespace kernel::memory::pmm {
/// @brief Picks the node an allocation is served from and falls back to remote nodes.
///
/// Every node gets a precomputed list of fallback nodes, sorted by memory tier (same tier first,
/// then slower, then faster), then SLIT distance, then bandwidth. Nodes further away than
/// MAX_ACCEPTABLE_LATENCY_NS are never used as a fallback.
///
/// A small bitmap remembers which nodes recently ran completely out of memory so that remote
/// fallbacks do not keep hammering their zone locks. It is only a hint: the preferred node is
/// always tried, a node is only marked when an order-0 request fails (larger requests fail on
/// fragmentation long before the node is empty), and the mark is dropped as soon as any page is
/// freed back to that node.
class Router {
 public:
  /// Upper bound on the SLIT distance of a fallback node (SLIT uses 10 for "local").
  static constexpr std::uint32_t MAX_ACCEPTABLE_LATENCY_NS = 800;

  struct FallbackRoute {
    std::uint32_t target_node;
    MemoryTier tier;
    std::uint32_t latency_ns;
    std::uint32_t bandwidth_mbps;
  };

 private:
  struct RoutingTableEntry {
    NumaNode* node;
    FallbackRoute* fallbacks;
    std::uint32_t fallback_count;
  };

  std::uint32_t m_num_active_nodes{0};
  RoutingTableEntry* m_routing_table{nullptr};
  FallbackRoute* m_fallback_matrix{nullptr};
  mutable std::atomic<std::uint64_t>* m_starved_nodes{nullptr};

  [[nodiscard, gnu::always_inline]] bool is_node_starved(
      const std::uint32_t node_id) const noexcept {
    const std::uint64_t mask = m_starved_nodes[node_id / 64].load(std::memory_order_relaxed);
    return (mask & (1ul << (node_id % 64))) != 0;
  }

  [[gnu::always_inline]] void mark_node_starved(const std::uint32_t node_id) const noexcept {
    m_starved_nodes[node_id / 64].fetch_or(1ul << (node_id % 64), std::memory_order_relaxed);
  }

  [[gnu::always_inline]] void clear_node_starved(const std::uint32_t node_id) const noexcept {
    m_starved_nodes[node_id / 64].fetch_and(~(1ul << (node_id % 64)), std::memory_order_release);
  }

  [[nodiscard]] Page* try_node(std::uint32_t node_id, PageMobility mobility,
                               std::uint8_t order) const noexcept;
  std::size_t fill_from_node(std::uint32_t node_id, PageMobility mobility, std::uint8_t order,
                             std::span<Page*> out) const noexcept;

 public:
  Router() = default;

  /// @brief Builds the routing table.
  /// @param active_nodes   Number of nodes, numbered densely from 0.
  /// @param nodes_array    The nodes, indexed by ID.
  /// @param slit_latencies Row-major `active_nodes x active_nodes` distance matrix.
  /// @param alloc          Boot allocator for the tables (they are never freed).
  void initialize(std::uint32_t active_nodes, NumaNode** nodes_array,
                  const std::uint32_t* slit_latencies, EarlyAllocFn alloc) noexcept;

  [[nodiscard]] std::uint32_t get_active_nodes() const noexcept { return m_num_active_nodes; }
  [[nodiscard]] NumaNode& get_node(const std::uint32_t node_id) const noexcept {
    return *m_routing_table[node_id].node;
  }

  /// @brief Allocates one block, preferring @p preferred_node.
  /// @return The head descriptor, or nullptr if no reachable node has a block of that size.
  [[nodiscard]] Page* alloc_single(std::uint32_t preferred_node, PageMobility mobility,
                                   std::uint8_t order) const noexcept;

  /// @brief Allocates up to `out_span.size()` blocks, preferring @p preferred_node.
  /// @return How many entries of @p out_span were filled.
  std::size_t alloc_bulk(std::uint32_t preferred_node, PageMobility mobility, std::uint8_t order,
                         std::span<Page*> out_span) const noexcept;

  /// @brief Returns a block to the node and zone recorded in its descriptor.
  void free_single(Page* page, std::uint8_t order) const noexcept;
};

inline Router g_router;
}  // namespace kernel::memory::pmm
