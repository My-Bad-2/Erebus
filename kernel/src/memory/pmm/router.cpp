#include "memory/pmm/router.hpp"

namespace kernel::memory::pmm {
void Router::build_fallback_matrix(const std::uint32_t *slit_latencies) const noexcept {
  for (std::uint32_t source_id = 0; source_id < m_num_active_nodes; ++source_id) {
    std::uint32_t fallback_idx = 0;
    MemoryTier source_tier = m_nodes[source_id]->get_tier();
    FallbackRoute *current_node_matrix = &m_fallback_matrix[source_id * m_num_active_nodes];

    for (std::uint32_t target_id = 0; target_id < m_num_active_nodes; ++target_id) {
      if (source_id == target_id) {
        continue;
      }

      const std::uint32_t latency = slit_latencies[source_id * m_num_active_nodes + target_id];
      if (latency > MAX_ACCEPTABLE_LATENCY_NS) {
        continue;
      }

      NumaNode *target = m_nodes[target_id];
      current_node_matrix[fallback_idx++] = {
          .target_node = target_id,
          .tier = target->get_tier(),
          .latency_ns = latency,
          .bandwidth_mbps = target->get_bandwidth(),
      };
    }

    m_fallback_counts[source_id] = fallback_idx;

    auto routes = std::span{current_node_matrix, fallback_idx};
    std::ranges::sort(routes, [source_tier](const FallbackRoute &a, const FallbackRoute &b) {
      auto get_tier_penalty = [source_tier](const MemoryTier t) -> int {
        if (t == source_tier) {
          return 0;
        }

        if (t > source_tier) {
          return 1;
        }

        return 2;
      };

      const int penalty_a = get_tier_penalty(a.tier);
      const int penalty_b = get_tier_penalty(b.tier);

      if (penalty_a != penalty_b) {
        return penalty_a < penalty_b;
      }

      if (a.latency_ns != b.latency_ns) {
        return a.latency_ns < b.latency_ns;
      }

      return a.bandwidth_mbps > b.bandwidth_mbps;
    });
  }
}

Page *Router::alloc_single(std::uint32_t preferred_node, const PageMobility mobility,
                           const std::uint8_t order) const noexcept {
  if (preferred_node >= m_num_active_nodes) [[unlikely]] {
    preferred_node = 0;
  }

  if (!is_node_starved(preferred_node)) [[likely]] {
    Page *page = m_nodes[preferred_node]->alloc_pages(mobility, order);
    if (page) [[likely]] {
      return page;
    }

    mark_node_starved(preferred_node);
  }

  const std::uint32_t fallback_count = m_fallback_counts[preferred_node];
  const FallbackRoute *fallback_row = &m_fallback_matrix[preferred_node * m_num_active_nodes];

  for (std::uint32_t i = 0; i < fallback_count; ++i) {
    const std::uint32_t remote_id = fallback_row[i].target_node;

    if (i + 1 < fallback_count) [[likely]] {
      const std::uint32_t next_id = fallback_row[i + 1].target_node;
      __builtin_prefetch(&fallback_row[i + 1], 0, 3);
      __builtin_prefetch(m_nodes[next_id], 0, 3);
    }

    if (is_node_starved(remote_id)) {
      continue;
    }

    Page *pg = m_nodes[remote_id]->alloc_pages(mobility, order);

    if (pg) [[likely]] {
      return pg;
    }

    mark_node_starved(remote_id);
  }

  return nullptr;
}

std::size_t Router::alloc_bulk(const std::uint32_t preferred_node, const PageMobility mobility,
                               const std::uint8_t order, std::span<Page *> out_span) const noexcept {
  std::size_t allocated = 0;
  while (allocated < out_span.size()) {
    Page *p = alloc_single(preferred_node, mobility, order);
    if (!p) {
      break;
    }

    out_span[allocated++] = p;
  }

  return allocated;
}

void Router::free_single(Page *page, const std::uint8_t order) const noexcept {
  const std::uint32_t node_id = page->get_numa_node();
  m_nodes[node_id]->get_zone(page->get_mobility()).free_page(page, order);
}
} // namespace kernel::memory::pmm