#include "memory/pmm.hpp"

#include <string.h>

#include "hal/smp/percpu.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/pmm/router.hpp"

namespace kernel::memory::pmm {
namespace {
// Carves memory off the top of one large usable region while the PMM is being built. Nothing
// it hands out is ever freed; the whole region is simply kept out of the buddy zones.
struct EarlyBumpAllocator {
  PhysicalAddress bottom{};
  PhysicalAddress top{};
  PhysicalAddress curr{};

  [[nodiscard]] VirtualAddress alloc(const std::size_t size, const std::size_t alignment) noexcept {
    if (size > static_cast<std::size_t>(curr - bottom)) [[unlikely]] {
      utils::logger::fatal("Early allocator ran out of memory!\n");
    }

    const PhysicalAddress next = (curr - size).align_down(alignment);
    if (next < bottom) [[unlikely]] {
      utils::logger::fatal("Early allocator ran out of memory!\n");
    }

    curr = next;
    const VirtualAddress virt = DirectMap::phys_to_virt(curr);
    klib::memset(virt.as<void>(), 0, size);
    return virt;
  }
};

EarlyBumpAllocator s_early_alloc;

VirtualAddress early_alloc(const std::size_t size, const std::size_t align) noexcept {
  return s_early_alloc.alloc(size, align);
}

template <typename T>
[[nodiscard]] T* early_array(const std::size_t count) noexcept {
  return early_alloc(sizeof(T) * count, alignof(T)).as<T>();
}

constexpr std::array ALL_MOBILITIES = {PageMobility::Unmovable, PageMobility::Movable,
                                       PageMobility::Reclaimable};

// Unmovable memory is fenced off at boot: 5% of each node, but never less than this.
constexpr std::uint64_t MIN_UNMOVABLE_BYTES = 64ul << 20;

struct NodeQuota {
  std::uint64_t usable_bytes;
  std::uint64_t unmovable_quota;
  std::uint64_t unmovable_injected;
};

constexpr const char* tier_to_string(const MemoryTier tier) noexcept {
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

constexpr const char* mobility_to_string(const PageMobility mobility) noexcept {
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

[[nodiscard]] bool is_tracked_type(const std::uint64_t type) noexcept {
  return type == LIMINE_MEMMAP_USABLE || type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
         type == LIMINE_MEMMAP_ACPI_RECLAIMABLE || type == LIMINE_MEMMAP_ACPI_NVS ||
         type == LIMINE_MEMMAP_BAD_MEMORY || type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
}

[[nodiscard]] Page* claim(Page* page) noexcept {
  page->ref_count.store(1, std::memory_order_relaxed);
  return page;
}

void mark_region(const PhysicalAddress start, const PhysicalAddress end,
                 const std::uint32_t node_id, const PageMobility mobility,
                 const PageState state) noexcept {
  for (PhysicalAddress c = start.align_down(PAGE_SIZE); c < end.align_up(PAGE_SIZE);
       c += PAGE_SIZE) {
    phys_to_page(c)->setup_buddy_metadata(state, mobility, 0, node_id);
  }
}

// The vmemmap has to cover every frame we might ever hand out or get asked about.
[[nodiscard]] std::uint64_t find_max_phys(const std::span<limine_memmap_entry*> memmap) noexcept {
  std::uint64_t max_phys_addr = 0;
  for (const auto* entry : memmap) {
    if (is_tracked_type(entry->type)) {
      max_phys_addr = std::max(entry->base + entry->length, max_phys_addr);
    }
  }

  return utils::maths::align_up(max_phys_addr, PAGE_SIZE);
}

// Picks the highest usable region that can hold the boot metadata.
void setup_early_allocator(const std::span<limine_memmap_entry*> memmap,
                           const std::size_t required_bytes) noexcept {
  for (auto it = memmap.rbegin(); it != memmap.rend(); ++it) {
    const auto* entry = *it;

    if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= required_bytes) {
      s_early_alloc.bottom = PhysicalAddress{utils::maths::align_up(entry->base, PAGE_SIZE)};
      s_early_alloc.top =
          PhysicalAddress{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)};
      s_early_alloc.curr = s_early_alloc.top;
      return;
    }
  }

  utils::logger::fatal("PMM: no usable region is large enough for the boot metadata!\n");
}

[[nodiscard]] NumaNode** create_nodes(const std::uint32_t total_cpus) noexcept {
  const std::uint32_t active_nodes = g_topology.active_nodes();
  auto** nodes = early_array<NumaNode*>(active_nodes);

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    const auto& [tier, bandwidth_mbps, base_latency_ns] = g_topology.get_node_metrics(i);
    void* node_mem = early_alloc(sizeof(NumaNode), alignof(NumaNode)).as<void>();
    nodes[i] = new (node_mem) NumaNode(i, tier, bandwidth_mbps, base_latency_ns);

    for (const PageMobility mobility : ALL_MOBILITIES) {
      nodes[i]->get_zone(mobility).allocate_pcp(early_array<PcpCache>(total_cpus));
    }
  }

  auto* slit_matrix = early_array<std::uint32_t>(active_nodes * active_nodes);
  for (std::uint32_t src = 0; src < active_nodes; ++src) {
    for (std::uint32_t tgt = 0; tgt < active_nodes; ++tgt) {
      slit_matrix[src * active_nodes + tgt] = g_topology.get_latency(src, tgt);
    }
  }

  g_router.initialize(active_nodes, nodes, slit_matrix, early_alloc);
  return nodes;
}

// Where does the chunk starting at `phys` end, and which node owns it? Memory outside every
// SRAT domain is attributed to node 0.
[[nodiscard]] std::pair<PhysicalAddress, std::uint32_t> chunk_bound(
    const PhysicalAddress phys, const PhysicalAddress max_end) noexcept {
  for (const auto& [base, end, node_id] : g_topology.domains()) {
    if (phys >= base && phys < end) {
      return {std::min(max_end, end), node_id};
    }

    if (base > phys) {
      return {std::min(max_end, base), 0};
    }
  }

  return {max_end, 0};
}

// Calls `fn(entry, start, end, node_id, is_boot_metadata)` for every piece of the memory map
// below `max_phys`, with pieces split at NUMA domain boundaries and around the region the early
// allocator used.
template <typename Fn>
void for_each_chunk(const std::span<limine_memmap_entry*> memmap, const PhysicalAddress max_phys,
                    Fn&& fn) noexcept {
  for (const auto* entry : memmap) {
    PhysicalAddress curr{utils::maths::align_up(entry->base, PAGE_SIZE)};
    const PhysicalAddress end =
        std::min(PhysicalAddress{utils::maths::align_down(entry->base + entry->length, PAGE_SIZE)},
                 max_phys);

    while (curr < end) {
      auto [chunk_end, node_id] = chunk_bound(curr, end);

      bool is_boot_metadata = false;
      if (curr < s_early_alloc.curr && chunk_end > s_early_alloc.curr) {
        chunk_end = s_early_alloc.curr;
      } else if (curr >= s_early_alloc.curr && curr < s_early_alloc.top) {
        chunk_end = std::min(chunk_end, s_early_alloc.top);
        is_boot_metadata = true;
      }

      fn(*entry, curr, chunk_end, node_id, is_boot_metadata);
      curr = chunk_end.align_up(PAGE_SIZE);
    }
  }
}

// Splits [start, end) into the largest naturally aligned blocks and hands them to the zones.
// The first `unmovable_quota` bytes of each node go to the unmovable zone, the rest is movable.
void inject_free_range(PhysicalAddress curr, const PhysicalAddress end, const std::uint32_t node_id,
                       NumaNode* node, NodeQuota& quota) noexcept {
  while (curr < end) {
    const std::uint64_t pfn = curr.value() / PAGE_SIZE;
    const std::uint64_t remaining_pages = (end.value() - curr.value()) / PAGE_SIZE;
    if (remaining_pages == 0) {
      return;
    }

    const auto align_order = static_cast<std::uint8_t>(std::min(std::countr_zero(pfn), 63));
    const auto size_order = static_cast<std::uint8_t>(std::bit_width(remaining_pages) - 1);
    std::uint8_t order = std::min({align_order, size_order, MAX_ORDER});

    auto mobility = PageMobility::Movable;
    if (quota.unmovable_injected < quota.unmovable_quota) {
      mobility = PageMobility::Unmovable;

      // Do not let a single 1 GiB block blow straight through the quota.
      const std::uint64_t missing_pages =
          utils::maths::div_round_up(quota.unmovable_quota - quota.unmovable_injected, PAGE_SIZE);
      order = std::min(order, static_cast<std::uint8_t>(std::bit_width(missing_pages) - 1));
      quota.unmovable_injected += PAGE_SIZE << order;
    }

    Page* head = pfn_to_page(pfn);
    head->setup_buddy_metadata(PageState::Free, mobility, order, node_id);
    node->get_zone(mobility).inject_free_page_cold(head, order);

    curr += static_cast<PhysicalAddress::OffsetType>(PAGE_SIZE << order);
  }
}
}  // namespace

namespace detail {
Page* g_vmemmap_base{nullptr};
std::uint64_t g_max_pfn{0};
}  // namespace detail

AllocResult alloc_pages(const PageMobility mobility, const std::uint8_t order) noexcept {
  if (order > MAX_ORDER) [[unlikely]] {
    return std::nullopt;
  }

  if (Page* page = g_router.alloc_single(hw::percpu::numa_node_id(), mobility, order)) [[likely]] {
    return page_to_phys(claim(page));
  }

  return std::nullopt;
}

AllocResult alloc_pages_zeroed(const PageMobility mobility, const std::uint8_t order) noexcept {
  if (const auto res = alloc_pages(mobility, order)) [[likely]] {
    klib::memset(DirectMap::phys_to_virt(*res).as<void>(), 0, PAGE_SIZE << order);
    return res;
  }

  return std::nullopt;
}

AllocResult alloc_pages_on_node(const std::uint32_t target_node, const PageMobility mobility,
                                const std::uint8_t order) noexcept {
  if (order > MAX_ORDER) [[unlikely]] {
    return std::nullopt;
  }

  if (Page* page = g_router.alloc_single(target_node, mobility, order)) [[likely]] {
    return page_to_phys(claim(page));
  }

  return std::nullopt;
}

std::size_t alloc_pages_bulk(const PageMobility mobility, const std::uint8_t order,
                             const std::span<PhysicalAddress> out_buffer) noexcept {
  if (out_buffer.empty() || order > MAX_ORDER) [[unlikely]] {
    return 0;
  }

  // The router works on descriptors; translate in fixed-size chunks to keep the stack small.
  constexpr std::size_t CHUNK_SIZE = 64;
  const std::uint32_t numa_node_id = hw::percpu::numa_node_id();

  std::array<Page*, CHUNK_SIZE> page_chunk{};
  std::size_t total_allocated = 0;

  while (total_allocated < out_buffer.size()) {
    const std::size_t batch_size = std::min(out_buffer.size() - total_allocated, CHUNK_SIZE);
    const std::size_t allocated = g_router.alloc_bulk(numa_node_id, mobility, order,
                                                      std::span{page_chunk.data(), batch_size});

    for (std::size_t i = 0; i < allocated; ++i) {
      out_buffer[total_allocated++] = page_to_phys(claim(page_chunk[i]));
    }

    if (allocated < batch_size) {
      break;
    }
  }

  return total_allocated;
}

void free_pages(const PhysicalAddress phys, const std::uint8_t order) noexcept {
  if (order > MAX_ORDER || !phys.is_aligned(PAGE_SIZE << order)) [[unlikely]] {
    utils::logger::fatal("PMM: free of {} with order {} is misaligned!\n", phys, order);
  }

  Page* page = phys_to_page(phys);

  const std::uint32_t prev = page->ref_count.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) [[likely]] {
    g_router.free_single(page, order);
  } else if (prev == 0) [[unlikely]] {
    utils::logger::fatal("PMM: double free of PFN {} ({})!\n", page_to_pfn(page), phys);
  }
}

void free_pages_bulk(const std::span<const PhysicalAddress> addresses,
                     const std::uint8_t order) noexcept {
  for (const auto& phys : addresses) {
    if (phys.value() != 0) [[likely]] {
      free_pages(phys, order);
    }
  }
}

void print_stats() noexcept {
  auto to_units = [](const std::uint64_t pages) -> std::pair<std::uint64_t, const char*> {
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

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    NumaNode& node = g_router.get_node(i);
    utils::logger::info("Node {} [{}]: {} MB/s | {} ns\n", i, tier_to_string(node.get_tier()),
                        node.get_bandwidth(), node.get_latency());

    std::uint64_t node_free_pages = 0;

    for (std::size_t z = 0; z < ALL_MOBILITIES.size(); ++z) {
      const PageMobility mobility = ALL_MOBILITIES[z];
      const MobilityZone& zone = node.get_zone(mobility);

      const std::uint64_t free_pages = zone.get_free_pages();
      const std::uint32_t frag_index = zone.calculate_fragmentation_index();
      const std::uint32_t active_bitmap = zone.get_active_orders_bitmap();
      const auto [min, low, high] = zone.get_watermarks();

      node_free_pages += free_pages;

      const bool is_last = (z == ALL_MOBILITIES.size() - 1);
      const char* prefix = is_last ? "    \\--" : "    |--";
      const char* wm_prefix = is_last ? "        " : "    |   ";

      const auto [val, unit] = to_units(free_pages);

      utils::logger::info("{} Zone {} | Free: {:>4} {} | Frag: {:>2}% | Bitmap: 0x{:05X}\n", prefix,
                          mobility_to_string(mobility), val, unit, frag_index, active_bitmap);

      if (min > 0) {
        const auto [min_v, min_u] = to_units(min);
        const auto [low_v, low_u] = to_units(low);
        const auto [high_v, high_u] = to_units(high);
        const char* health = (free_pages < low) ? "[WARNING: LOW]" : "[HEALTHY]";

        utils::logger::info("{} Watermarks: Min: {}{} | Low: {}{} | High: {}{} {}\n", wm_prefix,
                            min_v, min_u, low_v, low_u, high_v, high_u, health);
      }
    }

    global_free_pages += node_free_pages;
    const auto [node_val, node_unit] = to_units(node_free_pages);
    utils::logger::info("    Total Node Free: {} {}\n", node_val, node_unit);
  }

  const auto [global_val, global_unit] = to_units(global_free_pages);
  utils::logger::info("Global Free RAM: {} {}\n", global_val, global_unit);
}

void initialize(const std::span<limine_memmap_entry*> memmap,
                const std::uint32_t total_cpus) noexcept {
  const std::uint64_t max_phys_addr = find_max_phys(memmap);
  const std::uint64_t total_pages = max_phys_addr / PAGE_SIZE;
  detail::g_max_pfn = total_pages;

  // The vmemmap plus 16 MiB of headroom for the topology tables, nodes and per-CPU caches.
  const std::size_t vmemmap_size = total_pages * sizeof(Page);
  setup_early_allocator(memmap, vmemmap_size + (8 * PAGE_SIZE_2MB));

  // Zeroed, so every frame starts out Active with no references.
  detail::g_vmemmap_base = s_early_alloc.alloc(vmemmap_size, PAGE_SIZE).as<Page>();

  g_topology.parse(early_alloc);
  const std::uint32_t active_nodes = g_topology.active_nodes();
  NumaNode** nodes = create_nodes(total_cpus);

  // First pass: how much usable RAM does each node have? That sizes the unmovable quota.
  auto* quotas = early_array<NodeQuota>(active_nodes);
  const PhysicalAddress max_phys{max_phys_addr};

  for_each_chunk(memmap, max_phys,
                 [&](const limine_memmap_entry& entry, const PhysicalAddress start,
                     const PhysicalAddress end, const std::uint32_t node_id, const bool boot) {
                   if (entry.type == LIMINE_MEMMAP_USABLE && !boot) {
                     quotas[node_id].usable_bytes += end - start;
                   }
                 });

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    quotas[i].unmovable_quota = std::max(MIN_UNMOVABLE_BYTES, quotas[i].usable_bytes / 20);
  }

  // From here on the early allocator must not be used: its region is about to be marked as
  // permanently allocated and everything below it handed to the zones.
  for_each_chunk(memmap, max_phys,
                 [&](const limine_memmap_entry& entry, const PhysicalAddress start,
                     const PhysicalAddress end, const std::uint32_t node_id, const bool boot) {
                   if (boot) {
                     mark_region(start, end, node_id, PageMobility::Unmovable, PageState::Active);
                     return;
                   }

                   switch (entry.type) {
                     case LIMINE_MEMMAP_USABLE:
                       inject_free_range(start, end, node_id, nodes[node_id], quotas[node_id]);
                       break;
                     case LIMINE_MEMMAP_BAD_MEMORY:
                       mark_region(start, end, node_id, PageMobility::Unmovable, PageState::Bad);
                       break;
                     case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
                     case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
                       mark_region(start, end, node_id, PageMobility::Reclaimable,
                                   PageState::Active);
                       break;
                     default:
                       mark_region(start, end, node_id, PageMobility::Unmovable, PageState::Active);
                       break;
                   }
                 });

  for (std::uint32_t i = 0; i < active_nodes; ++i) {
    for (const PageMobility mobility : ALL_MOBILITIES) {
      MobilityZone& zone = nodes[i]->get_zone(mobility);
      zone.set_watermarks(zone.get_free_pages());

      // Empty zones still get tuned: pages stolen from their siblings will end up there.
      zone.tune_pcp(total_cpus);
    }
  }

  utils::logger::info("Physical Memory Manager Initialized!\n");
  print_stats();
}
}  // namespace kernel::memory::pmm
