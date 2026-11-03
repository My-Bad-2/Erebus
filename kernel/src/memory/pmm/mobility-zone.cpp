#include "memory/pmm/mobility-zone.hpp"

#include "memory/pmm.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/pmm/numa-node.hpp"

namespace kernel::memory::pmm {
namespace {
// Descriptors inside a block we own are always in range, so skip pfn_to_page()'s bounds check.
[[gnu::always_inline]] inline Page* page_at(const std::uint64_t pfn) noexcept {
  return detail::g_vmemmap_base + pfn;
}
}  // namespace

void MobilityZone::evaluate_watermarks(const std::uint64_t curr_free,
                                       const std::uint8_t requested_order,
                                       const bool alloc_failed) const noexcept {
  if (!g_daemons_active.load(std::memory_order_relaxed)) {
    return;
  }

  if (alloc_failed) {
    // Enough memory but no block big enough means fragmentation, not pressure.
    if (curr_free >= (1ul << requested_order)) {
      wake_compaction_daemon(m_parent_node->get_id());
    } else {
      wake_reclaim_daemon(m_parent_node->get_id());
    }
  } else if (curr_free < m_watermarks.low) [[unlikely]] {
    wake_reclaim_daemon(m_parent_node->get_id());
  }
}

void MobilityZone::flush_deferred_frees_internal() noexcept {
  if (m_deferred_frees.empty_approx()) {
    return;
  }

  std::uint64_t pages_freed = 0;
  Page* p = nullptr;
  while (m_deferred_frees.try_dequeue(p)) {
    const std::uint8_t order = p->get_order();
    buddy_merge_internal(p, order);
    pages_freed += 1ul << order;
  }

  add_free_pages(pages_freed);
}

void MobilityZone::buddy_merge_internal(Page* page, std::uint8_t order) noexcept {
  const std::uint32_t node_id = m_parent_node->get_id();
  std::uint64_t pfn = page_to_pfn(page);

  while (order < MAX_ORDER) {
    const std::uint64_t buddy_pfn = pfn ^ (1ul << order);

    // A block at the very top of memory may have a buddy that has no descriptor at all.
    if (buddy_pfn >= detail::g_max_pfn) {
      break;
    }

    // A buddy that is free with the same order is linked into the free list of *some* zone.
    // Mobility and node tell us whether it is ours; if it is not, unlinking it from our list
    // would corrupt both. Read the flags once so the three fields come from the same moment.
    Page* buddy = page_at(buddy_pfn);
    const Flags buddy_flags{buddy->flags.load(std::memory_order_acquire)};
    if (buddy_flags.get_state() != PageState::Free || buddy_flags.get_order() != order ||
        buddy_flags.get_mobility() != m_mobility || buddy->get_numa_node() != node_id) {
      break;
    }

    m_areas[order].remove(buddy);
    if (m_areas[order].is_empty()) {
      clear_order_active(order);
    }

    // The buddy is no longer a block head. If it turns out to be the lower half it becomes the
    // head of the merged block and is marked Free again below.
    buddy->set_state(PageState::Active);

    if (buddy_pfn < pfn) {
      page = buddy;
      pfn = buddy_pfn;
    }

    ++order;
  }

  page->setup_buddy_metadata(PageState::Free, m_mobility, order, node_id);
  m_areas[order].push_hot(page);
  set_order_active(order);
}

Page* MobilityZone::extract_block_internal(const std::uint8_t requested_order) noexcept {
  const std::uint32_t available = m_active_orders_bitmap & ~((1u << requested_order) - 1);
  if (available == 0) {
    return nullptr;
  }

  auto curr_order = static_cast<std::uint8_t>(std::countr_zero(available));
  Page* page = m_areas[curr_order].pop_hot();
  if (m_areas[curr_order].is_empty()) {
    clear_order_active(curr_order);
  }

  const std::uint32_t node_id = m_parent_node->get_id();
  const std::uint64_t pfn = page_to_pfn(page);

  // Keep the lower half each time and give the upper half back.
  while (curr_order > requested_order) {
    --curr_order;
    Page* upper = page_at(pfn + (1ul << curr_order));
    upper->setup_buddy_metadata(PageState::Free, m_mobility, curr_order, node_id);
    m_areas[curr_order].push_hot(upper);
    set_order_active(curr_order);
  }

  page->setup_buddy_metadata(PageState::Active, m_mobility, requested_order, node_id);
  return page;
}

std::uint32_t MobilityZone::calculate_fragmentation_index() const noexcept {
  const std::uint64_t free = get_free_pages();
  if (free == 0) {
    return 0;
  }

  const std::uint64_t superpage_pages = static_cast<std::uint64_t>(m_areas[MAX_ORDER].count())
                                        << MAX_ORDER;
  if (superpage_pages >= free) {
    return 0;
  }

  // e.g. 25% of free memory in MAX_ORDER blocks reads as 75% fragmented.
  return static_cast<std::uint32_t>(100 - ((superpage_pages * 100) / free));
}

Page* MobilityZone::alloc_pages_locked(const std::uint8_t requested_order) noexcept {
  flush_deferred_frees_internal();

  if (Page* page = extract_block_internal(requested_order)) [[likely]] {
    const std::uint64_t allocated = 1ul << requested_order;
    const std::uint64_t remaining_free =
        m_total_free_pages.fetch_sub(allocated, std::memory_order_relaxed) - allocated;
    evaluate_watermarks(remaining_free, requested_order, false);
    return page;
  }

  evaluate_watermarks(get_free_pages(), requested_order, true);
  return nullptr;
}

void MobilityZone::free_page_locked(Page* page, const std::uint8_t order) noexcept {
  flush_deferred_frees_internal();
  buddy_merge_internal(page, order);
  add_free_pages(1ul << order);
}

bool MobilityZone::batch_refill_pcp(PcpList& list, const std::uint8_t order) noexcept {
  utils::NakedGuard guard{m_lock};
  flush_deferred_frees_internal();

  // Only called on an empty list, and batch <= high <= capacity, so this cannot overflow it.
  const std::uint16_t batch = list.batch_size();
  std::uint32_t refilled = 0;
  while (refilled < batch) {
    Page* p = extract_block_internal(order);
    if (!p) {
      break;
    }

    list.push_hot(p);
    ++refilled;
  }

  if (refilled > 0) [[likely]] {
    const std::uint64_t pages_allocated = static_cast<std::uint64_t>(refilled) << order;
    const std::uint64_t remaining_free =
        m_total_free_pages.fetch_sub(pages_allocated, std::memory_order_relaxed) - pages_allocated;
    evaluate_watermarks(remaining_free, order, false);
    return true;
  }

  evaluate_watermarks(get_free_pages(), order, true);
  return false;
}

void MobilityZone::batch_drain_pcp(PcpList& list, const std::uint8_t order) noexcept {
  utils::NakedGuard guard{m_lock};
  flush_deferred_frees_internal();

  const std::uint16_t count = std::min(list.batch_size(), list.size());
  for (std::uint16_t i = 0; i < count; ++i) {
    buddy_merge_internal(list.pop_cold(), order);
  }

  add_free_pages(static_cast<std::uint64_t>(count) << order);
}

Page* MobilityZone::alloc_pages(const std::uint8_t requested_order) noexcept {
  if (requested_order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    return alloc_pages_locked(requested_order);
  }

  utils::IrqDisableGuard irq_guard;
  PcpList& list = m_pcp_cache[hw::percpu::id()].lists[requested_order];

  if (list.empty() && !batch_refill_pcp(list, requested_order)) [[unlikely]] {
    return nullptr;
  }

  return list.pop_hot();
}

void MobilityZone::free_page(Page* page, const std::uint8_t order) noexcept {
  if (hw::percpu::numa_node_id() != m_parent_node->get_id()) [[unlikely]] {
    // The deferred ring only carries the descriptor, so the order has to travel inside it.
    page->set_order(order);
    if (m_deferred_frees.try_enqueue(page)) {
      return;
    }

    utils::IrqSaveGuard guard{m_lock};
    free_page_locked(page, order);
    return;
  }

  if (order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    free_page_locked(page, order);
    return;
  }

  utils::IrqDisableGuard irq_guard;
  PcpList& list = m_pcp_cache[hw::percpu::id()].lists[order];

  if (list.needs_drain()) [[unlikely]] {
    batch_drain_pcp(list, order);
  }

  list.push_hot(page);
}

Page* MobilityZone::extract_largest_block_for_steal(const std::uint8_t min_order) noexcept {
  utils::IrqSaveGuard guard{m_lock};
  flush_deferred_frees_internal();

  const std::uint32_t available = m_active_orders_bitmap & ~((1u << min_order) - 1);
  if (available == 0) {
    return nullptr;
  }

  // Stealing the biggest block moves a whole region to the other mobility class at once, which
  // fragments both zones much less than nibbling small blocks off the victim.
  const auto order = static_cast<std::uint8_t>(std::bit_width(available) - 1);

  Page* page = m_areas[order].pop_hot();
  if (m_areas[order].is_empty()) {
    clear_order_active(order);
  }

  // Clear the Free mark while we still hold the lock: until the thief relabels the block, a
  // concurrent merge in this zone must not mistake it for a free buddy.
  page->set_state(PageState::Active);
  sub_free_pages(1ul << order);
  return page;
}

Page* MobilityZone::adopt_stolen_block(Page* block, const std::uint8_t block_order,
                                       const std::uint8_t requested_order) noexcept {
  utils::IrqSaveGuard guard{m_lock};

  const std::uint32_t node_id = m_parent_node->get_id();
  const std::uint64_t pfn = page_to_pfn(block);

  for (std::uint8_t order = block_order; order > requested_order;) {
    --order;
    Page* upper = page_at(pfn + (1ul << order));
    upper->setup_buddy_metadata(PageState::Free, m_mobility, order, node_id);
    m_areas[order].push_hot(upper);
    set_order_active(order);
  }

  block->setup_buddy_metadata(PageState::Active, m_mobility, requested_order, node_id);
  add_free_pages((1ul << block_order) - (1ul << requested_order));
  return block;
}
}  // namespace kernel::memory::pmm
