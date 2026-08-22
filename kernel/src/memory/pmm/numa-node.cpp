#include "memory/pmm/numa-node.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
Page *NumaNode::alloc_pages(const PageMobility requested_mobility, const std::uint8_t requested_order) noexcept {
  MobilityZone &primary_zone = get_zone(requested_mobility);

  Page *page = primary_zone.alloc_pages(requested_order);
  if (page) [[likely]] {
    return page;
  }

  const PageMobility *fallbacks = m_fallback_hierarchy[std::to_underlying(requested_mobility)];
  for (int i = 0; i < 2; ++i) {
    MobilityZone &fallback_zone = get_zone(fallbacks[i]);

    // Extract the largest possible block from the fallback zone
    Page *stolen_block = fallback_zone.extract_largest_block_for_steal(requested_order);
    if (stolen_block) {
      std::uint8_t stolen_order = stolen_block->get_order();

      utils::IrqSaveGuard guard{primary_zone.get_lock()};

      // Split the stolen block down to the requested order
      while (stolen_order > requested_order) {
        stolen_order--;
        const std::uint64_t buddy_pfn = page_to_pfn(stolen_block) ^ (1ul << stolen_order);
        Page *buddy = pfn_to_page(buddy_pfn);

        buddy->set_state(PageState::Free);
        buddy->set_order(stolen_order);
        buddy->set_mobility(requested_mobility);

        primary_zone.m_areas[stolen_order].push_hot(buddy);
        primary_zone.m_active_orders_bitmap |= (1u << stolen_order);
      }

      stolen_block->set_state(PageState::Active);
      stolen_block->set_mobility(requested_mobility);

      const std::uint64_t added_capacity = (1ul << stolen_block->get_order()) - (1ul << requested_order);
      primary_zone.m_total_free_pages.fetch_add(added_capacity, std::memory_order_relaxed);

      return stolen_block;
    }
  }

  // All local zones exhausted, steal from remote NumaNode
  return nullptr;
}
} // namespace kernel::memory::pmm