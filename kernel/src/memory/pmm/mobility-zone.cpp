#include "memory/pmm/mobility-zone.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/pmm/free_area.hpp"
#include "memory/pmm/numa-node.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
void MobilityZone::flush_deferred_frees_internal() noexcept {
  if (m_deferred_frees.empty_approx()) {
    return;
  }

  Page *p = nullptr;
  while (m_deferred_frees.try_dequeue(p)) {
    buddy_merge_internal(p, p->get_order());
  }
}

void MobilityZone::buddy_merge_internal(Page *page, std::uint8_t order) noexcept {
  std::uint64_t pfn = page_to_pfn(page);
  m_total_free_pages.fetch_add(1ul << order, std::memory_order_relaxed);

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

  page->set_state(PageState::Free);
  page->set_order(order);
  page->set_mobility(m_mobility);

  m_areas[order].push_hot(page);
  m_active_orders_bitmap |= (1u << order);
}

std::uint32_t MobilityZone::calculate_fragmentation_index() const noexcept {
  const std::uint64_t free = m_total_free_pages.load(std::memory_order_relaxed);
  if (free == 0) {
    return 0;
  }

  const std::uint64_t superpage_capacity = m_areas[MAX_ORDER].count() * (1ul << MAX_ORDER);
  return static_cast<std::uint32_t>(100 - ((superpage_capacity * 100) / free));
}

Page *MobilityZone::alloc_pages_locked(const std::uint8_t requested_order) noexcept {
  flush_deferred_frees_internal();

  const std::uint32_t search_mask = ~((1u << requested_order) - 1);
  const std::uint32_t available = m_active_orders_bitmap & search_mask;

  if (available != 0) [[likely]] {
    std::uint8_t curr_order = std::countr_zero(available);

    Page *page = m_areas[curr_order].pop_hot();
    if (m_areas[curr_order].is_empty()) {
      m_active_orders_bitmap &= ~(1u << curr_order);
    }

    // split cascade
    while (curr_order > requested_order) {
      curr_order--;

      std::uint64_t buddy_pfn = page_to_pfn(page) ^ (1ul << curr_order);
      Page *buddy = pfn_to_page(buddy_pfn);

      buddy->set_state(PageState::Free);
      buddy->set_order(curr_order);
      buddy->set_mobility(m_mobility);

      m_areas[curr_order].push_hot(buddy);
      m_active_orders_bitmap |= (1u << curr_order);
    }

    page->set_state(PageState::Active);

    const std::uint64_t remaining_free =
        m_total_free_pages.fetch_sub(1ul << requested_order, std::memory_order_relaxed) - (1ul << requested_order);

    if (g_daemons_active.load(std::memory_order_relaxed)) {
      if (remaining_free < m_watermarks.low) [[unlikely]] {
        wake_reclaim_daemon(m_parent_node->get_id());
      }
    }

    return page;
  }

  if (g_daemons_active.load(std::memory_order_relaxed)) {
    if (get_free_pages() >= (1ul << requested_order)) {
      wake_compaction_daemon(m_parent_node->get_id());
    } else {
      wake_reclaim_daemon(m_parent_node->get_id());
    }
  }

  return nullptr;
}

void MobilityZone::free_page_locked(Page *page, const std::uint8_t order) noexcept {
  flush_deferred_frees_internal();
  buddy_merge_internal(page, order);
}

bool MobilityZone::scavenge_deferred_frees(PcpList &list) noexcept {
  std::uint32_t scavenged = 0;
  Page *p = nullptr;

  while (scavenged < list.batch && m_deferred_frees.try_dequeue(p)) {
    list.push_hot(p);
    scavenged++;
  }

  return scavenged > 0;
}

bool MobilityZone::batch_refill_pcp(PcpList &list, const std::uint8_t order) noexcept {
  utils::IrqSaveGuard guard{m_lock};
  std::uint32_t refilled = 0;

  while (refilled < list.batch) {
    Page *p = alloc_pages_locked(order);
    if (!p) {
      break;
    }

    list.push_hot(p);
    refilled++;
  }

  return refilled > 0;
}

void MobilityZone::batch_drain_pcp(PcpList &list, std::uint8_t order) noexcept {
  utils::IrqSaveGuard guard{m_lock};

  for (std::uint32_t i = 0; i < list.batch; ++i) {
    Page *cold_page = list.pop_cold();
    free_page_locked(cold_page, order);
  }
}

Page *MobilityZone::alloc_pages(const std::uint8_t requested_order) noexcept {
  if (requested_order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    return alloc_pages_locked(requested_order);
  }

  utils::IrqDisableGuard irq_guard;
  std::uint32_t cpu_id = hw::percpu::id();
  PcpList &list = m_pcp_cache[cpu_id].lists[requested_order];

  if (!list.deque.empty()) [[likely]] {
    return list.pop_hot();
  }

  if (requested_order == 0 && scavenge_deferred_frees(list)) [[likely]] {
    return list.pop_hot();
  }

  if (batch_refill_pcp(list, requested_order)) [[likely]] {
    return list.pop_hot();
  }

  return nullptr; // OOM
}

void MobilityZone::free_page(Page *page, const std::uint8_t order) noexcept {
  const std::uint32_t curr_node_id = hw::percpu::numa_node();
  const std::uint32_t cpu_id = hw::percpu::id();

  if (curr_node_id != m_parent_node->get_id()) [[unlikely]] {
    if (m_deferred_frees.try_enqueue(page)) {
      return;
    }
  }

  if (order > PCP_MAX_ORDER || m_pcp_cache == nullptr) [[unlikely]] {
    utils::IrqSaveGuard guard{m_lock};
    free_page_locked(page, order);
    return;
  }

  utils::IrqDisableGuard irq_guard;
  PcpList &list = m_pcp_cache[cpu_id].lists[order];

  if (list.deque.size() < list.high) [[likely]] {
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
  const std::uint8_t target_order = 63 - std::countl_zero(available);

  Page *page = m_areas[target_order].pop_hot();
  if (m_areas[target_order].is_empty()) {
    m_active_orders_bitmap &= ~(1u << target_order);
  }

  m_total_free_pages.fetch_sub(1ul << target_order, std::memory_order_relaxed);
  // The caller must split this page and change its mobility
  return page;
}
} // namespace kernel::memory::pmm