#include "memory/pmm/mobility-zone.hpp"
#include "memory/pmm.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/pmm/free_area.hpp"
#include "memory/pmm/numa-node.hpp"

namespace kernel::memory::pmm {
void MobilityZone::evaluate_watermarks(const std::uint64_t curr_free, const std::uint8_t requested_order,
                                       const bool alloc_failed) const noexcept {
  if (!g_daemons_active.load(std::memory_order_relaxed)) {
    return;
  }

  if (alloc_failed) {
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
  Page *p = nullptr;
  while (m_deferred_frees.try_dequeue(p)) {
    const std::uint8_t order = p->get_order();
    buddy_merge_internal(p, p->get_order());
    pages_freed += (1ul << order);
  }

  if (pages_freed > 0) {
    m_total_free_pages.fetch_add(pages_freed, std::memory_order_relaxed);
  }
}

void MobilityZone::buddy_merge_internal(Page *page, std::uint8_t order) noexcept {
  std::uint64_t pfn = page_to_pfn(page);

  while (order < MAX_ORDER) {
    const std::uint64_t buddy_pfn = pfn ^ (1ul << order);
    Page *buddy = pfn_to_page(buddy_pfn);

    if (buddy->get_state() != PageState::Free || buddy->get_order() != order || buddy->get_mobility() != m_mobility) {
      break; // Buddy is active, wrong size, or wrong mobility zone
    }

    m_areas[order].remove(buddy);
    if (m_areas[order].is_empty()) {
      m_active_orders_bitmap &= ~(1u << order);
    }

    if (buddy_pfn < pfn) {
      page = buddy;
      pfn = buddy_pfn;
    }

    order++;
  }

  page->setup_buddy_metadata(PageState::Free, m_mobility, order, m_parent_node->get_id());
  m_areas[order].push_hot(page);
  m_active_orders_bitmap |= (1u << order);
}

Page *MobilityZone::extract_block_internal(const std::uint8_t requested_order) noexcept {
  const std::uint32_t search_mask = ~((1u << requested_order) - 1);
  const std::uint32_t available = m_active_orders_bitmap & search_mask;

  if (available == 0) {
    return nullptr;
  }

  std::uint8_t curr_order = static_cast<std::uint8_t>(std::countr_zero(available));
  Page *page = m_areas[curr_order].pop_hot();

  if (m_areas[curr_order].is_empty()) {
    m_active_orders_bitmap &= ~(1u << curr_order);
  }

  while (curr_order > requested_order) {
    curr_order--;
    const std::uint64_t buddy_pfn = page_to_pfn(page) ^ (1ul << curr_order);
    Page *buddy = pfn_to_page(buddy_pfn);

    page->setup_buddy_metadata(PageState::Free, m_mobility, curr_order, m_parent_node->get_id());

    m_areas[curr_order].push_hot(buddy);
    m_active_orders_bitmap |= (1u << curr_order);
  }

  page->set_state(PageState::Active);
  page->set_order(curr_order);
  return page;
}

std::uint32_t MobilityZone::calculate_fragmentation_index() const noexcept {
  const std::uint64_t free = m_total_free_pages.load(std::memory_order_relaxed);
  if (free == 0) {
    return 0; // Empty Zone, no fragmentation
  }

  const std::uint64_t superpage_pages = static_cast<std::uint64_t>(m_areas[MAX_ORDER].count()) << MAX_ORDER;
  if (superpage_pages >= free) {
    return 0; // 0% fragmented (all available memory is perfectly contiguous
  }

  // If 25% of free RAM is in superpages, the zone is 75% fragmented
  return static_cast<std::uint32_t>(100 - ((superpage_pages * 100) / free));
}

Page *MobilityZone::alloc_pages_locked(const std::uint8_t requested_order) noexcept {
  flush_deferred_frees_internal();

  if (Page *page = extract_block_internal(requested_order)) [[likely]] {
    const std::uint64_t allocated = 1ul << requested_order;
    const std::uint64_t remaining_free = m_total_free_pages.fetch_sub(allocated, std::memory_order_relaxed) - allocated;
    evaluate_watermarks(remaining_free, requested_order, false);
    return page;
  }

  evaluate_watermarks(get_free_pages(), requested_order, true);
  return nullptr;
}

void MobilityZone::free_page_locked(Page *page, const std::uint8_t order) noexcept {
  flush_deferred_frees_internal();
  buddy_merge_internal(page, order);
  m_total_free_pages.fetch_add(1ul << order, std::memory_order_relaxed);
}

bool MobilityZone::batch_refill_pcp(PcpList &list, const std::uint8_t order) noexcept {
  utils::NakedGuard guard{m_lock};
  flush_deferred_frees_internal();

  std::uint32_t refilled = 0;
  while (refilled < list.batch_size()) {
    Page *p = extract_block_internal(order);
    if (!p) {
      break;
    }

    list.push_hot(p);
    refilled++;
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

void MobilityZone::batch_drain_pcp(PcpList &list, std::uint8_t order) noexcept {
  utils::NakedGuard guard{m_lock};
  flush_deferred_frees_internal();

  for (std::uint32_t i = 0; i < list.batch_size(); ++i) {
    Page *cold_page = list.pop_cold();
    buddy_merge_internal(cold_page, order);
  }

  m_total_free_pages.fetch_add(static_cast<std::uint64_t>(list.batch_size()) << order, std::memory_order_relaxed);
}

Page *MobilityZone::alloc_pages(const std::uint8_t requested_order) noexcept {
  if (requested_order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    return alloc_pages_locked(requested_order);
  }

  utils::IrqDisableGuard irq_guard;
  std::uint32_t cpu_id = hw::percpu::id();
  PcpList &list = m_pcp_cache[cpu_id].lists[requested_order];

  if (!list.empty()) [[likely]] {
    return list.pop_hot();
  }

  if (batch_refill_pcp(list, requested_order)) [[likely]] {
    return list.pop_hot();
  }

  return nullptr; // OOM
}

void MobilityZone::free_page(Page *page, const std::uint8_t order) noexcept {
  const std::uint32_t curr_node_id = hw::percpu::numa_node_id();
  const std::uint32_t cpu_id = hw::percpu::id();

  if (curr_node_id != m_parent_node->get_id()) [[unlikely]] {
    if (!m_deferred_frees.try_enqueue(page)) {
      utils::IrqSaveGuard guard{m_lock};
      free_page_locked(page, order);
    }

    return;
  }

  if (order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    free_page_locked(page, order);
    return;
  }

  utils::IrqDisableGuard irq_guard;
  PcpList &list = m_pcp_cache[cpu_id].lists[order];

  if (list.needs_drain()) [[likely]] {
    list.push_hot(page);
    return;
  }

  batch_drain_pcp(list, order);
  list.push_hot(page);
}

Page *MobilityZone::extract_largest_block_for_steal(const std::uint8_t min_order) noexcept {
  utils::IrqSaveGuard guard{m_lock};
  flush_deferred_frees_internal();

  const std::uint32_t search_mask = ~((1u << min_order) - 1);
  const std::uint32_t available = m_active_orders_bitmap & search_mask;

  if (available == 0) {
    return nullptr;
  }

  // Find the highest available order to steal.
  const std::uint8_t target_order = static_cast<std::uint8_t>(std::bit_width(available) - 1);

  Page *page = m_areas[target_order].pop_hot();
  if (m_areas[target_order].is_empty()) {
    m_active_orders_bitmap &= ~(1u << target_order);
  }

  m_total_free_pages.fetch_sub(1ul << target_order, std::memory_order_relaxed);
  return page;
}
} // namespace kernel::memory::pmm