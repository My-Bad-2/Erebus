#include "memory/pmm/numa-node.hpp"

#include "memory/pmm.hpp"

namespace kernel::memory::pmm {
Page* NumaNode::alloc_pages(const PageMobility requested_mobility,
                            const std::uint8_t requested_order) noexcept {
  MobilityZone& primary = get_zone(requested_mobility);
  if (Page* page = primary.alloc_pages(requested_order)) [[likely]] {
    return page;
  }

  for (const PageMobility fallback : s_fallback_hierarchy[std::to_underlying(requested_mobility)]) {
    MobilityZone& victim = get_zone(fallback);

    Page* stolen = victim.extract_largest_block_for_steal(requested_order);
    if (!stolen) {
      continue;
    }

    // The two zone locks are taken one after the other, never nested: the block is marked
    // Active while in transit, so neither zone can see it as a free buddy in between.
    const std::uint8_t stolen_order = stolen->get_order();
    victim.evaluate_watermarks(victim.get_free_pages(), stolen_order, false);
    return primary.adopt_stolen_block(stolen, stolen_order, requested_order);
  }

  return nullptr;
}
}  // namespace kernel::memory::pmm
