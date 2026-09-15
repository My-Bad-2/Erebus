#include "memory/pmm/router.hpp"

namespace kernel::memory::pmm {
void Router::initialize(std::uint32_t active_nodes, NumaNode **nodes_array, const std::uint32_t *slit_latencies,
                        EarlyAllocFn alloc) noexcept {
  m_num_active_nodes = active_nodes;

  m_routing_table = alloc(sizeof(RoutingTableEntry) * active_nodes, alignof(RoutingTableEntry)).as<RoutingTableEntry>();
  m_fallback_matrix =
      alloc(sizeof(FallbackRoute) * active_nodes * active_nodes, alignof(FallbackRoute)).as<FallbackRoute>();

  const std::uint32_t mask_elems = (active_nodes + 63) / 64;
  m_starved_nodes = alloc(sizeof(std::atomic<std::uint64_t>) * mask_elems, std::hardware_destructive_interference_size)
                        .as<std::atomic<std::uint64_t>>();

  for (std::uint32_t i = 0; i < mask_elems; ++i) {
    m_starved_nodes[i].store(0, std::memory_order_relaxed);
  }

  for (std::uint32_t source_id = 0; source_id < active_nodes; ++source_id) {
    std::uint32_t fallback_idx = 0;
    NumaNode *source_node = nodes_array[source_id];
    MemoryTier source_tier = source_node->get_tier();

    FallbackRoute *current_node_matrix = &m_fallback_matrix[source_id * active_nodes];

    for (std::uint32_t target_id = 0; target_id < active_nodes; ++target_id) {
      if (source_id == target_id)
        continue;

      const std::uint32_t latency = slit_latencies[source_id * active_nodes + target_id];
      if (latency > MAX_ACCEPTABLE_LATENCY_NS)
        continue;

      NumaNode *target = nodes_array[target_id];
      current_node_matrix[fallback_idx++] = {
          .target_node = target_id,
          .tier = target->get_tier(),
          .latency_ns = latency,
          .bandwidth_mbps = target->get_bandwidth(),
      };
    }

    auto routes = std::span{current_node_matrix, fallback_idx};
    std::ranges::sort(routes, [source_tier](const FallbackRoute &a, const FallbackRoute &b) {
      auto get_tier_penalty = [source_tier](const MemoryTier t) -> int {
        if (t == source_tier)
          return 0;
        return (t > source_tier) ? 1 : 2;
      };

      const int penalty_a = get_tier_penalty(a.tier);
      const int penalty_b = get_tier_penalty(b.tier);

      if (penalty_a != penalty_b)
        return penalty_a < penalty_b;
      if (a.latency_ns != b.latency_ns)
        return a.latency_ns < b.latency_ns;
      return a.bandwidth_mbps > b.bandwidth_mbps;
    });

    m_routing_table[source_id] = {
        .node = source_node, .fallbacks = current_node_matrix, .fallback_count = fallback_idx};
  }
}

Page *Router::alloc_single(std::uint32_t preferred_node, const PageMobility mobility,
                           const std::uint8_t order) const noexcept {
  if (preferred_node >= m_num_active_nodes) [[unlikely]] {
    preferred_node = 0;
  }

  const RoutingTableEntry &route = m_routing_table[preferred_node];

  if (!is_node_starved(preferred_node)) [[likely]] {
    if (Page *page = route.node->alloc_pages(mobility, order)) [[likely]] {
      return page;
    }

    mark_node_starved(preferred_node);
  }

  for (std::uint32_t i = 0; i < route.fallback_count; ++i) {
    const std::uint32_t remote_id = route.fallbacks[i].target_node;

    if (i + 1 < route.fallback_count) [[likely]] {
      __builtin_prefetch(&route.fallbacks[i + 1], 0, 3);
      __builtin_prefetch(m_routing_table[route.fallbacks[i + 1].target_node].node, 0, 3);
    }

    if (is_node_starved(remote_id)) {
      continue;
    }

    if (Page *pg = m_routing_table[remote_id].node->alloc_pages(mobility, order)) [[likely]] {
      return pg;
    }

    mark_node_starved(remote_id);
  }

  return nullptr;
}

std::size_t Router::alloc_bulk(const std::uint32_t preferred_node, const PageMobility mobility,
                               const std::uint8_t order, std::span<Page *> out_span) const noexcept {
  if (out_span.empty())
    return 0;

  std::size_t allocated = 0;
  const std::uint32_t target_node = preferred_node >= m_num_active_nodes ? 0 : preferred_node;

  // Closure to pull pages from a specific node until it runs out or we fulfill the span
  auto exhaust_node = [&](const std::uint32_t nid) -> bool {
    if (is_node_starved(nid)) {
      return false;
    }

    MobilityZone &zone = m_routing_table[nid].node->get_zone(mobility);
    utils::IrqSaveGuard guard{zone.get_lock()};

    while (allocated < out_span.size()) {
      Page *p = zone.alloc_pages_locked(order);
      if (!p) {
        mark_node_starved(nid);
        return false; // Zone exhausted
      }

      out_span[allocated++] = p;
    }

    return true; // Span completely fulfilled
  };

  // Try Primary
  if (exhaust_node(target_node)) {
    return allocated;
  }

  // Cascade through fallbacks to fulfill the remainder
  const RoutingTableEntry &route = m_routing_table[target_node];
  for (std::uint32_t i = 0; i < route.fallback_count; ++i) {
    if (exhaust_node(route.fallbacks[i].target_node)) {
      break;
    }
  }

  if (allocated < out_span.size()) {
    clear_node_starved(target_node);
  }

  return allocated;
}

void Router::free_single(Page *page, const std::uint8_t order) const noexcept {
  const std::uint32_t node_id = page->get_numa_node();

  if (is_node_starved(node_id)) {
    clear_node_starved(node_id);
  }

  m_routing_table[node_id].node->get_zone(page->get_mobility()).free_page(page, order);
}
} // namespace kernel::memory::pmm