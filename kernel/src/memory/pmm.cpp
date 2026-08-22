#include "memory/pmm.hpp"
#include "hal/percpu.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/pmm/router.hpp"

#include "utils/logger.hpp"

#include <string.h>

namespace kernel::memory::pmm {
namespace {
Page *g_vmemmap_base;

PhysicalAddress s_early_top{};
PhysicalAddress s_early_bottom{};
PhysicalAddress s_early_curr{};

constexpr const char *tier_to_string(MemoryTier tier) noexcept {
  switch (tier) {
  case MemoryTier::HBM:
    return "HBM";
  case MemoryTier::DDR:
    return "DDR";
  case MemoryTier::CXL_LOCAL:
    return "CXL";
  case MemoryTier::PMEM:
    return "PME";
  default:
    return "UNK";
  }
}

constexpr const char *mobility_to_string(PageMobility mobility) noexcept {
  switch (mobility) {
  case PageMobility::Unmovable:
    return "Unmovable  ";
  case PageMobility::Movable:
    return "Movable    ";
  case PageMobility::Reclaimable:
    return "Reclaimable";
  default:
    return "Unknown    ";
  }
}

[[nodiscard]] VirtualAddress early_alloc_top_down(std::size_t size, std::size_t alignment) noexcept {
  const PhysicalAddress ptr = (s_early_curr - size).align_down(alignment);
  if (ptr < s_early_bottom) [[unlikely]] {
    utils::logger::fatal("Early allocator ran out of memory!\n");
  }

  s_early_curr = ptr;

  const VirtualAddress virt_ptr = DirectMap::phys_to_virt(ptr);
  klib::memset(virt_ptr.as<void>(), 0, size);
  return virt_ptr;
}
} // namespace

std::uint64_t page_to_pfn(const Page *page) noexcept { return static_cast<std::uint64_t>(page - g_vmemmap_base); }
Page *pfn_to_page(const std::uint64_t pfn) noexcept { return g_vmemmap_base + pfn; }

AllocResult alloc_pages(const PageMobility mobility, const std::uint8_t order) noexcept {
  const std::uint32_t numa_node_id = hw::percpu::numa_node();
  Page *pg = g_router.alloc_single(numa_node_id, mobility, order);

  if (pg) [[likely]] {
    return page_to_phys(pg);
  }

  return std::nullopt;
}

AllocResult alloc_pages_zeroed(const PageMobility mobility, const std::uint8_t order) noexcept {
  const auto res = alloc_pages(mobility, order);

  if (res.has_value()) [[likely]] {
    const VirtualAddress virt = DirectMap::phys_to_virt(*res);
    const std::size_t size = PAGE_SIZE << order;
    klib::memset(virt.as<void>(), 0, size);
  }

  return res;
}

AllocResult alloc_pages_on_node(const std::uint32_t target_node, const PageMobility mobility,
                                const std::uint8_t order) noexcept {
  Page *page = g_router.alloc_single(target_node, mobility, order);
  if (page) [[likely]] {
    return page_to_phys(page);
  }

  return std::nullopt;
}

inline std::size_t alloc_pages_bulk(const PageMobility mobility, const std::uint8_t order,
                                    std::span<PhysicalAddress> out_buffer) noexcept {
  constexpr std::size_t CHUNK_SIZE = 64;
  const std::uint32_t numa_node_id = hw::percpu::numa_node();

  Page *page_chunk[CHUNK_SIZE];

  std::size_t total_allocated = 0;
  std::size_t remaining = out_buffer.size();

  while (remaining > 0) {
    const std::size_t batch_size = std::min(remaining, CHUNK_SIZE);
    const std::size_t allocated = g_router.alloc_bulk(numa_node_id, mobility, order, std::span{page_chunk, batch_size});

    for (std::size_t i = 0; i < allocated; ++i) {
      out_buffer[total_allocated++] = page_to_phys(page_chunk[i]);
    }

    if (allocated < batch_size) {
      break;
    }

    remaining -= batch_size;
  }

  return total_allocated;
}

void free_pages(const PhysicalAddress phys, const std::uint8_t order) noexcept {
  if (!phys.is_aligned(PAGE_SIZE << order)) [[unlikely]] {
    utils::logger::fatal("Attempted to free unaligned memory!\n");
  }

  Page *page = phys_to_page(phys);
  g_router.free_single(page, order);
}

void free_pages_bulk(const std::span<const PhysicalAddress> addresses, const std::uint8_t order) noexcept {
  for (const auto &phys : addresses) {
    if (phys.value() != 0) [[likely]] {
      free_pages(phys, order);
    }
  }
}

void print_stats() noexcept {
  std::uint64_t global_free_pages = 0;
  const std::uint32_t active_nodes = g_router.get_active_nodes();

  utils::logger::info("PMM Statistics:\n");
  utils::logger::info("NUMA Topology: {} Active Node(s)\n", active_nodes);

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    NumaNode *node = &g_router.get_node(i);
    if (!node) {
      continue;
    }

    utils::logger::info("Node {} [{}]: {} MB/s | {} ns\n", i, tier_to_string(node->get_tier()), node->get_bandwidth(),
                        node->get_latency());

    std::uint64_t node_free_pages = 0;

    for (std::uint8_t m = 0; m < 3; ++m) {
      MobilityZone &zone = node->get_zone(PageMobility{m});

      const std::uint64_t free_pages = zone.get_free_pages();
      const std::uint32_t frag_index = zone.calculate_fragmentation_index();
      const std::uint32_t active_bitmap = zone.get_active_orders_bitmap();
      const auto [min, low, high] = zone.get_watermarks();

      node_free_pages += free_pages;
      const char *prefix = (m == 2) ? "    \\--" : "    |--";

      utils::logger::info("{} Zone {} | Free: {:>5} MB | Frag: {:>2}% | Bitmap: 0x{:05X}\n", prefix,
                          mobility_to_string(PageMobility{m}), (free_pages * PAGE_SIZE) / (1024 * 1024), frag_index,
                          active_bitmap);

      if (min > 0) {
        const char *health = (free_pages < low) ? "[WARNING: LOW]" : "[HEALTHY]";
        utils::logger::info("        Watermarks: Min: {} | Low: {} | High: {} {}\n", min, low, high, health);
      }
    }

    global_free_pages += node_free_pages;
    utils::logger::info("    Total Node Free: {} MB\n", (node_free_pages * PAGE_SIZE) / (1024 * 1024));
  }

  utils::logger::info("Global Free RAM: {} MB\n", (global_free_pages * PAGE_SIZE) / (1024 * 1024));
}

void initialize(std::span<limine_memmap_entry *> memmap, const std::uint32_t total_cpus) noexcept {
  std::uint64_t max_phys_addr = 0;

  // Find he highest valid phys address
  for (const auto *entry : memmap) {
    switch (entry->type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
    case LIMINE_MEMMAP_BAD_MEMORY: {
      std::uint64_t top = entry->base + entry->length;
      max_phys_addr = std::max(top, max_phys_addr);
      break;
    }
    default:
      break;
    }
  }

  // Align up to ensure the vmemmap array covers any partial trailing page
  max_phys_addr = utils::maths::align_up(max_phys_addr, PAGE_SIZE);

  // Calculate max vmemmap size + 16MB buffer for SRAT/HMAT routing topologies
  const std::uint64_t total_pages = max_phys_addr / PAGE_SIZE;
  const std::size_t required_metadata_size = (total_pages * sizeof(Page)) + (8 * PAGE_SIZE_2MB);

  // Setup the early bump allocator
  for (auto it = memmap.rbegin(); it != memmap.rend(); ++it) {
    const auto *entry = *it;

    if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= required_metadata_size) {
      s_early_bottom = PhysicalAddress{utils::maths::align_up(entry->base, PAGE_SIZE)};
      s_early_top = PhysicalAddress{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};
      s_early_curr = s_early_top;
      break;
    }
  }

  if (s_early_curr.value() == 0) {
    utils::logger::fatal("Early allocator ran out of memory!\n");
  }

  // Reserve memory for vmemmap
  const std::size_t vmemmap_size = total_pages * sizeof(Page);
  VirtualAddress vmemmap_virt = early_alloc_top_down(vmemmap_size, PAGE_SIZE);
  g_vmemmap_base = vmemmap_virt.as<Page>();

  // Parse ACPI topology
  TopologyParser topology;
  topology.parse(early_alloc_top_down);

  const std::uint32_t active_nodes = topology.active_nodes();
  auto **nodes = early_alloc_top_down(sizeof(NumaNode *) * active_nodes, alignof(NumaNode *)).as<NumaNode *>();
  auto *fallbacks =
      early_alloc_top_down(sizeof(Router::FallbackRoute) * active_nodes * active_nodes, 8).as<Router::FallbackRoute>();
  auto *counts = early_alloc_top_down(sizeof(std::uint32_t) * active_nodes, 4).as<std::uint32_t>();

  const std::uint32_t mask_elems = (active_nodes + 63) / 64;
  auto *starv_mask =
      early_alloc_top_down(sizeof(std::atomic<std::uint64_t>) * mask_elems, 64).as<std::atomic<std::uint64_t>>();

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    VirtualAddress node_mem = early_alloc_top_down(sizeof(NumaNode), alignof(NumaNode));
    const auto &[tier, bandwidth_mbps, base_latency_ns] = topology.get_node_metrics(i);
    nodes[i] = new (node_mem.as<void>()) NumaNode(i, tier, bandwidth_mbps, base_latency_ns);

    // Allocate PCP caches for all 3 zones
    for (std::uint8_t m = 0; m < 3; ++m) {
      VirtualAddress pcp_mem = early_alloc_top_down(sizeof(PcpCache) * total_cpus, alignof(PcpCache));

      nodes[i]->get_zone(PageMobility{m}).allocate_pcp(pcp_mem.as<void>());
    }
  }

  auto *slit_matrix = early_alloc_top_down(sizeof(std::uint32_t) * active_nodes * active_nodes, 4).as<std::uint32_t>();
  for (std::uint32_t src = 0; src < active_nodes; ++src) {
    for (std::uint32_t tgt = 0; tgt < active_nodes; ++tgt) {
      slit_matrix[src * active_nodes + tgt] = topology.get_latency(src, tgt);
    }
  }

  g_router.inject_topology(active_nodes, nodes, counts, fallbacks, starv_mask);
  g_router.build_fallback_matrix(slit_matrix);

  const PhysicalAddress max_tracked_phys{max_phys_addr};
  const PhysicalAddress early_metadata_start = s_early_curr.align_down(PAGE_SIZE);

  for (const auto *entry : memmap) {
    PhysicalAddress curr_phys{utils::maths::align_up(entry->base, PAGE_SIZE)};
    PhysicalAddress end_phys{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};

    if (end_phys > max_tracked_phys) {
      end_phys = max_tracked_phys;
    }

    // If the region is smaller than a page, or unaligned garbage, skip it.
    if (curr_phys >= end_phys) {
      continue;
    }

    if (entry->type != LIMINE_MEMMAP_USABLE) {
      while (curr_phys < end_phys) {
        Page *p = phys_to_page(curr_phys);
        p->set_state(PageState::Active);

        switch (entry->type) {
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
          p->set_mobility(PageMobility::Movable);
          break;
        case LIMINE_MEMMAP_BAD_MEMORY:
          p->set_mobility(PageMobility::Unmovable);
          break;
        default:
          p->set_mobility(PageMobility::Unmovable);
          p->set_cpu_owner(0);
          break;
        }

        curr_phys += PAGE_SIZE;
      }

      continue;
    }

    // Handle usable RAM
    while (curr_phys < end_phys) {
      // Isolate Early Bump Allocator memory
      if (curr_phys >= s_early_curr && curr_phys < s_early_top) {
        const PhysicalAddress isolation_end = std::min(end_phys, s_early_top);

        while (curr_phys < isolation_end) {
          Page *p = phys_to_page(curr_phys);
          p->set_state(PageState::Active);
          p->set_mobility(PageMobility::Unmovable);
          p->set_cpu_owner(0);
          curr_phys += PAGE_SIZE;
        }

        continue;
      }

      std::uint32_t target_node_id = 0;
      PhysicalAddress boundary_end = end_phys;
      bool found_domain = false;

      for (const auto &[base, end, node_id] : topology.domains()) {
        if (curr_phys >= base && curr_phys < end) {
          target_node_id = node_id;
          PhysicalAddress domain_end_aligned{utils::maths::align_down(end.value(), PAGE_SIZE)};
          boundary_end = std::min(end_phys, domain_end_aligned);
          found_domain = true;
          break;
        }
      }

      // If we're in a gap between SRAT domains, clamp the boundary to the start of the next domain.
      if (!found_domain) {
        for (const auto &[base, end, node_id] : topology.domains()) {
          if (base > curr_phys) {
            PhysicalAddress domain_base_aligned{utils::maths::align_down(base.value(), PAGE_SIZE)};
            boundary_end = std::min(end_phys, domain_base_aligned);
            break;
          }
        }
      }

      // Clamp by bump allocator region if we're approaching it from below
      if (boundary_end > early_metadata_start && curr_phys < early_metadata_start) {
        boundary_end = early_metadata_start;
      }

      // Prevent indefinite loops if boundaries overlap or squeeze
      if (curr_phys >= boundary_end) {
        curr_phys = curr_phys.align_up(PAGE_SIZE);

        if (curr_phys == boundary_end) {
          curr_phys += PAGE_SIZE;
        }

        continue;
      }

      MobilityZone &target_zone = nodes[target_node_id]->get_zone(PageMobility::Movable);

      while (curr_phys < boundary_end) {
        const std::uint64_t current_pfn = curr_phys.value() / PAGE_SIZE;
        const std::uint64_t remaining_pages = (boundary_end.value() - curr_phys.value()) / PAGE_SIZE;

        if (remaining_pages == 0) [[unlikely]] {
          curr_phys = boundary_end.align_up(PAGE_SIZE);
          break;
        }

        const std::uint8_t align_order = static_cast<std::uint8_t>(std::countr_zero(current_pfn));
        const std::uint8_t size_order = static_cast<std::uint8_t>(std::bit_width(remaining_pages)) - 1;
        const std::uint8_t order = std::min({align_order, size_order, MAX_ORDER});

        Page *head_page = pfn_to_page(current_pfn);
        head_page->set_state(PageState::Free);
        head_page->set_order(order);
        head_page->set_mobility(PageMobility::Movable);
        head_page->topology.set_mut<"numa_node">(target_node_id);

        target_zone.inject_free_page_cold(head_page, order);
        curr_phys += static_cast<std::uint64_t>(PAGE_SIZE) << order;
      }
    }
  }

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    for (std::uint8_t m = 0; m < 3; ++m) {
      MobilityZone &zone = nodes[i]->get_zone(PageMobility{m});
      const std::uint64_t ingested_pages = zone.get_free_pages();

      if (ingested_pages > 0) [[unlikely]] {
        zone.set_watermarks(ingested_pages);
        zone.tune_pcp(total_cpus);
      }
    }
  }

  utils::logger::info("Physical Memory Manager Initialized!\n");
  print_stats();
}
} // namespace kernel::memory::pmm