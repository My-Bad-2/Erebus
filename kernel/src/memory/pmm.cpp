#include "memory/pmm.hpp"
#include "hal/percpu.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/pmm/router.hpp"

#include <string.h>

namespace kernel::memory::pmm {
namespace {
struct EarlyBumpAllocator {
  PhysicalAddress bottom{};
  PhysicalAddress top{};
  PhysicalAddress curr{};

  [[nodiscard]] VirtualAddress alloc(std::size_t size, std::size_t alignment) noexcept {
    curr = (curr - size).align_down(alignment);
    if (curr < bottom) [[unlikely]] {
      utils::logger::fatal("Early allocator ran out of memory!\n");
    }

    VirtualAddress virt = DirectMap::phys_to_virt(curr);
    klib::memset(virt.as<void>(), 0, size);
    return virt;
  }
};

EarlyBumpAllocator s_early_alloc;

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

void mark_active_region(const PhysicalAddress start, const PhysicalAddress end, const std::uint32_t node_id,
                        const PageMobility mobility) noexcept {
  const PhysicalAddress aligned_start = start.align_down(PAGE_SIZE);
  const PhysicalAddress aligned_end = end.align_up(PAGE_SIZE);

  for (PhysicalAddress c = aligned_start; c < aligned_end; c += PAGE_SIZE) {
    Page *p = phys_to_page(c);
    p->setup_buddy_metadata(PageState::Active, mobility, 0, node_id);
  }
}
} // namespace

namespace detail {
Page *g_vmemmap_base{nullptr};
std::uint64_t g_max_pfn;
} // namespace detail

AllocResult alloc_pages(const PageMobility mobility, const std::uint8_t order) noexcept {
  const std::uint32_t numa_node_id = hw::percpu::numa_node();

  if (Page *page = g_router.alloc_single(numa_node_id, mobility, order)) [[likely]] {
    page->ref_count.store(1, std::memory_order_relaxed);
    return page_to_phys(page);
  }

  return std::nullopt;
}

AllocResult alloc_pages_zeroed(const PageMobility mobility, const std::uint8_t order) noexcept {
  if (const auto res = alloc_pages(mobility, order)) [[likely]] {
    const VirtualAddress virt = DirectMap::phys_to_virt(*res);
    klib::memset(virt.as<void>(), 0, PAGE_SIZE << order);
    return res;
  }

  return std::nullopt;
}

AllocResult alloc_pages_on_node(const std::uint32_t target_node, const PageMobility mobility,
                                const std::uint8_t order) noexcept {
  if (Page *pg = g_router.alloc_single(target_node, mobility, order)) [[likely]] {
    pg->ref_count.store(1, std::memory_order_relaxed);
    return page_to_phys(pg);
  }

  return std::nullopt;
}

std::size_t alloc_pages_bulk(const PageMobility mobility, const std::uint8_t order,
                             std::span<PhysicalAddress> out_buffer) noexcept {
  if (out_buffer.empty()) [[unlikely]] {
    return 0;
  }

  constexpr std::size_t CHUNK_SIZE = 64;
  const std::uint32_t numa_node_id = hw::percpu::numa_node();

  std::array<Page *, CHUNK_SIZE> page_chunk{};
  std::size_t total_allocated = 0;

  while (total_allocated < out_buffer.size()) {
    const std::size_t batch_size = std::min(out_buffer.size() - total_allocated, CHUNK_SIZE);
    const std::size_t allocated =
        g_router.alloc_bulk(numa_node_id, mobility, order, std::span{page_chunk.data(), batch_size});

    for (std::size_t i = 0; i < allocated; ++i) {
      page_chunk[i]->ref_count.store(1, std::memory_order_relaxed);
      out_buffer[total_allocated++] = page_to_phys(page_chunk[i]);
    }

    if (allocated < batch_size) {
      break; // Global OOM condition hit midway
    }
  }

  return total_allocated;
}

void free_pages(const PhysicalAddress phys, const std::uint8_t order) noexcept {
  if (!phys.is_aligned(static_cast<std::uint64_t>(PAGE_SIZE) << order)) [[unlikely]] {
    utils::logger::fatal("Attempted to free unaligned memory!\n");
  }

  Page *page = phys_to_page(phys);

  if (page->get_state() == PageState::Free) [[unlikely]] {
    utils::logger::fatal("Double free detected on PFN %llu!\n", page_to_pfn(page));
  }

  if (page->dec_ref_and_test()) {
    g_router.free_single(page, order);
  }
}

void free_pages_bulk(const std::span<const PhysicalAddress> addresses, const std::uint8_t order) noexcept {
  for (const auto &phys : addresses) {
    if (phys.value() != 0) [[likely]] {
      free_pages(phys, order);
    }
  }
}

void print_stats() noexcept {
  auto to_units = [](std::uint64_t pages) -> std::pair<std::uint64_t, const char *> {
    const std::uint64_t bytes = pages * PAGE_SIZE;
    if (bytes >= (1ul << 30)) {
      return {bytes >> 30, "GB"};
    }

    if (bytes >= (1ul << 20)) {
      return {bytes >> 20, "MB"};
    }

    if (bytes >= (1ul << 10)) {
      return {bytes >> 10, "KB"};
    }

    return {bytes, " B"};
  };

  std::uint64_t global_free_pages = 0;
  const std::uint32_t active_nodes = g_router.get_active_nodes();

  utils::logger::info("PMM Statistics:\n");
  utils::logger::info("NUMA Topology: {} Active Node(s)\n", active_nodes);

  constexpr std::array zone_types = {PageMobility::Unmovable, PageMobility::Movable, PageMobility::Reclaimable};

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    NumaNode &node = g_router.get_node(i);
    utils::logger::info("Node {} [{}]: {} MB/s | {} ns\n", i, tier_to_string(node.get_tier()), node.get_bandwidth(),
                        node.get_latency());

    std::uint64_t node_free_pages = 0;

    for (std::size_t z = 0; z < zone_types.size(); ++z) {
      const PageMobility mobility = zone_types[z];
      MobilityZone &zone = node.get_zone(mobility);

      const std::uint64_t free_pages = zone.get_free_pages();
      const std::uint32_t frag_index = zone.calculate_fragmentation_index();
      const std::uint32_t active_bitmap = zone.get_active_orders_bitmap();
      const auto [min, low, high] = zone.get_watermarks();

      node_free_pages += free_pages;

      const bool is_last = (z == zone_types.size() - 1);
      const char *prefix = is_last ? "    \\--" : "    |--";
      const char *wm_prefix = is_last ? "        " : "    |   ";

      const auto [val, unit] = to_units(free_pages);

      utils::logger::info("{} Zone {} | Free: {:>4} {} | Frag: {:>2}% | Bitmap: 0x{:05X}\n", prefix,
                          mobility_to_string(mobility), val, unit, frag_index, active_bitmap);

      if (min > 0) {
        const auto [min_v, min_u] = to_units(min);
        const auto [low_v, low_u] = to_units(low);
        const auto [high_v, high_u] = to_units(high);
        const char *health = (free_pages < low) ? "[WARNING: LOW]" : "[HEALTHY]";

        utils::logger::info("{} Watermarks: Min: {}{} | Low: {}{} | High: {}{} {}\n", wm_prefix, min_v, min_u, low_v,
                            low_u, high_v, high_u, health);
      }
    }

    global_free_pages += node_free_pages;
    const auto [node_val, node_unit] = to_units(node_free_pages);
    utils::logger::info("    Total Node Free: {} {}\n", node_val, node_unit);
  }

  const auto [global_val, global_unit] = to_units(global_free_pages);
  utils::logger::info("Global Free RAM: {} {}\n", global_val, global_unit);
}

void initialize(std::span<limine_memmap_entry *> memmap, const std::uint32_t total_cpus) noexcept {
  std::uint64_t max_phys_addr = 0;

  // Find the highest valid physical address
  for (const auto *entry : memmap) {
    if (entry->type == LIMINE_MEMMAP_USABLE || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE || entry->type == LIMINE_MEMMAP_ACPI_NVS ||
        entry->type == LIMINE_MEMMAP_BAD_MEMORY || entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
      const std::uint64_t top = entry->base + entry->length;
      max_phys_addr = std::max(top, max_phys_addr);
    }
  }

  // Align up to ensure the vmemmap array covers any partial trailing page
  max_phys_addr = utils::maths::align_up(max_phys_addr, PAGE_SIZE);

  // Calculate max vmemmap size + 16MB buffer for SRAT/HMAT routing topologies
  const std::uint64_t total_pages = max_phys_addr / PAGE_SIZE;
  const std::size_t required_metadata_size = (total_pages * sizeof(Page)) + (8 * PAGE_SIZE_2MB);
  detail::g_max_pfn = total_pages;

  // Setup the early bump allocator
  for (auto it = memmap.rbegin(); it != memmap.rend(); ++it) {
    const auto *entry = *it;

    if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= required_metadata_size) {
      s_early_alloc.bottom = PhysicalAddress{utils::maths::align_up(entry->base, PAGE_SIZE)};
      s_early_alloc.top = PhysicalAddress{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};
      s_early_alloc.curr = s_early_alloc.top;
      break;
    }
  }

  if (s_early_alloc.curr.value() == 0) {
    utils::logger::fatal("Early allocator ran out of memory!\n");
  }

  // Reserve memory for vmemmap
  const std::size_t vmemmap_size = total_pages * sizeof(Page);
  detail::g_vmemmap_base = s_early_alloc.alloc(vmemmap_size, PAGE_SIZE).as<Page>();

  g_topology.parse([](const std::size_t size, const std::size_t align) { return s_early_alloc.alloc(size, align); });

  const std::uint32_t active_nodes = g_topology.active_nodes();
  auto **nodes = s_early_alloc.alloc(sizeof(NumaNode *) * active_nodes, alignof(NumaNode *)).as<NumaNode *>();

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    VirtualAddress node_mem = s_early_alloc.alloc(sizeof(NumaNode), alignof(NumaNode));
    const auto &[tier, bandwidth_mbps, base_latency_ns] = g_topology.get_node_metrics(i);

    nodes[i] = new (node_mem.as<void>()) NumaNode(i, tier, bandwidth_mbps, base_latency_ns);

    for (std::uint8_t m = 0; m < 3; ++m) {
      VirtualAddress pcp_mem = s_early_alloc.alloc(sizeof(PcpCache) * total_cpus, alignof(PcpCache));
      nodes[i]->get_zone(PageMobility{m}).allocate_pcp(pcp_mem.as<void>());
    }
  }

  auto *slit_matrix = s_early_alloc.alloc(sizeof(std::uint32_t) * active_nodes * active_nodes, 4).as<std::uint32_t>();
  for (std::uint32_t src = 0; src < active_nodes; ++src) {
    for (std::uint32_t tgt = 0; tgt < active_nodes; ++tgt) {
      slit_matrix[src * active_nodes + tgt] = g_topology.get_latency(src, tgt);
    }
  }

  g_router.initialize(active_nodes, nodes, slit_matrix,
                      [](const std::size_t size, const std::size_t align) { return s_early_alloc.alloc(size, align); });

  // Snaps physical addresses to the nearest NUMA domain
  auto get_chunk_bound = [&](const PhysicalAddress phys,
                             const PhysicalAddress max_end) -> std::pair<PhysicalAddress, std::uint32_t> {
    for (const auto &[base, end, node_id] : g_topology.domains()) {
      if (phys >= base && phys < end) {
        return {std::min(max_end, end), node_id};
      }

      if (base > phys) { // In a gap before this domain
        return {std::min(max_end, base), 0};
      }
    }

    return {max_end, 0}; // Past all domains
  };

  auto *node_usable_bytes = s_early_alloc.alloc(sizeof(std::uint64_t) * active_nodes, 8).as<std::uint64_t>();
  auto *node_unmovable_quota = s_early_alloc.alloc(sizeof(std::uint64_t) * active_nodes, 8).as<std::uint64_t>();
  auto *node_unmovable_injected = s_early_alloc.alloc(sizeof(std::uint64_t) * active_nodes, 8).as<std::uint64_t>();

  for (const auto *entry : memmap) {
    if (entry->type != LIMINE_MEMMAP_USABLE)
      continue;

    PhysicalAddress curr{utils::maths::align_up(entry->base, PAGE_SIZE)};
    PhysicalAddress end_phys{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};
    end_phys = std::min(end_phys, PhysicalAddress{max_phys_addr});

    while (curr < end_phys) {
      auto [chunk_end, node_id] = get_chunk_bound(curr, end_phys);
      // Intersect with Early Allocator bounds to exclude metadata from the quota
      if (curr < s_early_alloc.curr && chunk_end > s_early_alloc.curr) {
        chunk_end = s_early_alloc.curr;
      } else if (curr >= s_early_alloc.curr && curr < s_early_alloc.top) {
        chunk_end = std::min(chunk_end, s_early_alloc.top);
        curr = chunk_end.align_up(PAGE_SIZE);
        continue;
      }

      node_usable_bytes[node_id] += (chunk_end.value() - curr.value());
      curr = chunk_end.align_up(PAGE_SIZE);
    }
  }

  // Calculate strict quotas: 5% of Node RAM, absolute minimum 64MB
  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    node_unmovable_quota[i] = std::max<std::uint64_t>(32 * PAGE_SIZE_2MB, node_usable_bytes[i] / 20);
    node_unmovable_injected[i] = 0;
  }

  for (const auto *entry : memmap) {
    PhysicalAddress curr{utils::maths::align_up(entry->base, PAGE_SIZE)};
    PhysicalAddress end_phys{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};
    end_phys = std::min(end_phys, PhysicalAddress{max_phys_addr});

    while (curr < end_phys) {
      auto [chunk_end, node_id] = get_chunk_bound(curr, end_phys);

      // Intersect with Early Allocator Used Memory
      if (curr < s_early_alloc.curr && chunk_end > s_early_alloc.curr) {
        chunk_end = s_early_alloc.curr;
      } else if (curr >= s_early_alloc.curr && curr < s_early_alloc.top) {
        chunk_end = std::min(chunk_end, s_early_alloc.top);
        mark_active_region(curr, chunk_end, node_id, PageMobility::Unmovable);
        curr = chunk_end.align_up(PAGE_SIZE);
        continue;
      }

      // Handle Firmware / Reserved Memory
      if (entry->type != LIMINE_MEMMAP_USABLE) {
        auto mob = PageMobility::Unmovable;
        if (entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
          mob = PageMobility::Reclaimable;
        }

        mark_active_region(curr, chunk_end, node_id, mob);
        curr = chunk_end.align_up(PAGE_SIZE);
        continue;
      }

      // Inject Free Usable RAM into Buddy Zones
      while (curr < chunk_end) {
        const std::uint64_t current_pfn = curr.value() / PAGE_SIZE;
        const std::uint64_t remaining_pages = (chunk_end.value() - curr.value()) / PAGE_SIZE;

        if (remaining_pages == 0) [[unlikely]] {
          mark_active_region(curr, chunk_end, node_id, PageMobility::Unmovable);
          curr = chunk_end.align_up(PAGE_SIZE);
          break;
        }

        const std::uint8_t align_order = static_cast<std::uint8_t>(std::countr_zero(current_pfn));
        const std::uint8_t size_order = static_cast<std::uint8_t>(std::countr_zero(std::bit_floor(remaining_pages)));
        const std::uint8_t order = std::min({align_order, size_order, MAX_ORDER});

        // Zone Policy: Lower 64MB reserved for Unmovable.
        const std::uint64_t block_bytes = static_cast<std::uint64_t>(PAGE_SIZE) << order;
        auto assigned_mobility = PageMobility::Movable;

        if (node_unmovable_injected[node_id] < node_unmovable_quota[node_id]) {
          assigned_mobility = PageMobility::Unmovable;
          node_unmovable_injected[node_id] += block_bytes;
        }

        Page *head_page = pfn_to_page(current_pfn);
        head_page->setup_buddy_metadata(PageState::Free, assigned_mobility, order, node_id);
        nodes[node_id]->get_zone(assigned_mobility).inject_free_page_cold(head_page, order);

        curr += PhysicalAddress{static_cast<std::uint64_t>(PAGE_SIZE) << order};
      }
    }
  }

  // Tune PCP caches and wake daemons based on ingested memory
  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    for (std::uint8_t m = 0; m < 3; ++m) {
      MobilityZone &zone = nodes[i]->get_zone(PageMobility{m});
      const std::uint64_t ingested_pages = zone.get_free_pages();

      if (ingested_pages > 0) {
        zone.set_watermarks(ingested_pages);
        zone.tune_pcp(total_cpus);
      }
    }
  }

  utils::logger::info("Physical Memory Manager Initialized!\n");
  print_stats();
}
} // namespace kernel::memory::pmm