#pragma once

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
  std::uint32_t m_num_active_nodes{0};
  NumaNode **m_nodes{nullptr};
  FallbackRoute *m_fallback_matrix{nullptr};
  std::uint32_t *m_fallback_counts{nullptr};

  // Size = ceil (active_nodes / 64)
  std::atomic<std::uint64_t> *m_starved_nodes_mask{nullptr};

  [[nodiscard, gnu::always_inline]] bool is_node_starved(const std::uint32_t node_id) const noexcept {
    const std::uint64_t mask = m_starved_nodes_mask[node_id / 64].load(std::memory_order_relaxed);
    return (mask & (1ul << (node_id % 64))) != 0;
  }

  [[gnu::always_inline]] void mark_node_starved(const std::uint32_t node_id) const noexcept {
    m_starved_nodes_mask[node_id / 64].fetch_or(1ul << (node_id % 64), std::memory_order_relaxed);
  }

public:
  Router() = default;

  void inject_topology(const std::uint32_t active_nodes, NumaNode **nodes_array, std::uint32_t *fallback_counts_array,
                       FallbackRoute *fallback_matrix_array,
                       std::atomic<std::uint64_t> *starvation_mask_array) noexcept {
    m_num_active_nodes = active_nodes;
    m_nodes = nodes_array;
    m_fallback_counts = fallback_counts_array;
    m_fallback_matrix = fallback_matrix_array;
    m_starved_nodes_mask = starvation_mask_array;

    const std::uint32_t mask_elements = (active_nodes + 63) / 64;
    for (std::uint32_t i = 0; i < mask_elements; ++i) {
      m_starved_nodes_mask[i].store(0, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::uint32_t get_active_nodes() const noexcept { return m_num_active_nodes; }
  [[nodiscard]] NumaNode &get_node(const std::uint32_t node_id) const noexcept { return *m_nodes[node_id]; }

  void build_fallback_matrix(const std::uint32_t *slit_latencies) const noexcept;
  [[nodiscard]] Page *alloc_single(std::uint32_t preferred_node, PageMobility mobility,
                                   std::uint8_t order) const noexcept;
  std::size_t alloc_bulk(std::uint32_t preferred_node, PageMobility mobility, std::uint8_t order,
                         std::span<Page *> out_span) const noexcept;
  void free_single(Page *page, std::uint8_t order) const noexcept;
};

inline Router g_router;
} // namespace kernel::memory::pmm