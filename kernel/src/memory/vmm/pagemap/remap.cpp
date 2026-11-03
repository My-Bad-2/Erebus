#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "memory/vmm/pagemap/tlb.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> PageMap::remap_1gb_as_2mb(const VirtualAddress virt,
                                                     const PhysicalAddress new_phys,
                                                     const AccessFlags flags, const CacheMode cache,
                                                     const std::uint8_t pkey) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB) || !new_phys.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, false, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  PageTableEntry* pdpte = *pdpte_res;

  const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
  if (!new_page_res) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  const PhysicalAddress new_pd_phys{*new_page_res};
  auto* pd_table = DirectMap::phys_to_virt(new_pd_phys).as<PageTableEntry>();
  const AccessFlags lflags = leaf_flags(virt, flags);

  for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
    const PhysicalAddress offset_phys =
        new_phys + static_cast<PhysicalAddress::OffsetType>(i * PAGE_SIZE_2MB);
    pd_table[i].store(
        PageTableEntry::build_schema(offset_phys, lflags, cache, PageSize::Size2M, pkey),
        std::memory_order_relaxed);
  }

  const PTEntry desired = PageTableEntry::build_directory(new_pd_phys, !virt.is_upper_half());
  PTEntry expected = pdpte->load(std::memory_order_acquire);

  while (true) {
    if (!expected.get_present()) [[unlikely]] {
      pmm::free_pages(new_pd_phys, 0);
      return std::unexpected(Error::NotMapped);
    }

    if (expected.get_huge()) [[unlikely]] {
      pmm::free_pages(new_pd_phys, 0);
      return std::unexpected(Error::InvalidFlags);
    }

    if (pdpte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire))
        [[likely]] {
      break;
    }
  }

  tlb::ShootdownCoordinator::broadcast(this, virt, PAGE_SIZE_1GB, PageSize::Size2M);

  // TODO: Defer this with RCU once lockless walkers exist; today every walker that could still
  // be inside the old directory is serialised by the VMA layer.
  pmm::free_pages(PhysicalAddress{expected.get_pfn_4k() << PAGE_SHIFT_4KB}, 0);
  return {};
}

std::expected<void, Error> PageMap::remap(const VirtualAddress virt, const PhysicalAddress new_phys,
                                          const AccessFlags flags, const CacheMode cache,
                                          const std::uint8_t pkey, const PageSize size) noexcept {
  if (virt.is_upper_half() && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  const std::size_t page_size_bytes = size_to_bytes(size);
  if (!virt.is_aligned(page_size_bytes) || !new_phys.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !g_paging_features.huge_1g) [[unlikely]] {
    return remap_1gb_as_2mb(virt, new_phys, flags, cache, pkey);
  }

  const auto pte_res = walk(virt, false, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry* pte = *pte_res;
  const PTEntry desired =
      PageTableEntry::build_schema(new_phys, leaf_flags(virt, flags), cache, size, pkey);
  PTEntry expected = pte->load(std::memory_order_acquire);

  while (true) {
    if (!expected.get_present()) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    if (pte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire))
        [[likely]] {
      tlb::ShootdownCoordinator::broadcast(this, virt, page_size_bytes, size);
      return {};
    }
  }
}

namespace {
// One page worth of original entries, so a failed multi-page update can be put back. The
// entries of a node are consecutive slots of one table, starting at base_virt.
constexpr std::size_t MAX_LOG_PTES = 508;

struct UndoLogNode {
  UndoLogNode* prev;
  VirtualAddress base_virt;
  std::uint32_t count;
  PageSize page_size;
  PTEntry original_ptes[MAX_LOG_PTES];
};

static_assert(sizeof(UndoLogNode) <= PAGE_SIZE);
}  // namespace

// Owns the undo log of a range operation. On failure it restores every logged entry, newest
// first; either way it frees the log. This assumes nobody else modifies the range meanwhile,
// which the VMA layer guarantees by holding its lock.
struct RollbackGuard {
  PageMap& pmap;
  UndoLogNode* tail_log{nullptr};
  bool success{false};
  VirtualAddress start_virt;
  std::size_t attempted_bytes{0};

  RollbackGuard(PageMap& map, const VirtualAddress start) noexcept : pmap(map), start_virt(start) {}

  RollbackGuard(const RollbackGuard&) = delete;
  RollbackGuard& operator=(const RollbackGuard&) = delete;

  /// @brief Starts a new log node for entries beginning at @p base_virt.
  [[nodiscard]] UndoLogNode* begin(const VirtualAddress base_virt, const PageSize size) noexcept {
    const auto log_page = pmm::alloc_pages(pmm::PageMobility::Unmovable, 0);
    if (!log_page) [[unlikely]] {
      return nullptr;
    }

    auto* node = DirectMap::phys_to_virt(*log_page).as<UndoLogNode>();
    node->prev = tail_log;
    node->base_virt = base_virt;
    node->count = 0;
    node->page_size = size;
    tail_log = node;
    return node;
  }

  ~RollbackGuard() {
    if (!success && tail_log) {
      for (const UndoLogNode* node = tail_log; node; node = node->prev) {
        if (node->count == 0) {
          continue;
        }

        if (auto pt_res = pmap.walk(node->base_virt, false, node->page_size)) {
          PageTableEntry* pte = *pt_res;
          for (std::uint32_t i = 0; i < node->count; ++i) {
            pte[i].store(node->original_ptes[i], std::memory_order_release);
          }
        }
      }

      if (attempted_bytes > 0) {
        tlb::ShootdownCoordinator::broadcast(&pmap, start_virt, attempted_bytes);
      }
    }

    while (tail_log) {
      UndoLogNode* prev = tail_log->prev;
      if (const auto phys = DirectMap::virt_to_phys(VirtualAddress{tail_log})) {
        pmm::free_pages(*phys, 0);
      }

      tail_log = prev;
    }
  }
};

std::expected<void, Error> PageMap::remap_range(const VirtualAddress start_virt,
                                                const PhysicalAddress start_phys,
                                                const std::size_t size_bytes,
                                                const AccessFlags flags, const CacheMode cache,
                                                const std::uint8_t pkey) noexcept {
  if (start_virt.is_upper_half() && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  if (!start_virt.is_aligned(PAGE_SIZE) || !start_phys.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  RollbackGuard guard{*this, start_virt};
  const AccessFlags lflags = leaf_flags(start_virt, flags);

  VirtualAddress curr_virt = start_virt;
  PhysicalAddress curr_phys = start_phys;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    const auto trans = translate(curr_virt);
    if (!trans) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    // Keep the current page size where the new range is aligned for it, otherwise go down a
    // size (walk() shatters on the way).
    PageSize target_size = PageSize::Size4K;
    for (const PageSize size : {PageSize::Size1G, PageSize::Size2M}) {
      const std::size_t bytes = size_to_bytes(size);
      if (trans->mapped_size >= size && remaining >= bytes && curr_virt.is_aligned(bytes) &&
          curr_phys.is_aligned(bytes)) {
        target_size = size;
        break;
      }
    }

    const std::uint8_t target_lvl = std::to_underlying(target_size);
    const std::size_t step_size = size_to_bytes(target_size);
    const std::size_t table_coverage = level_coverage(target_lvl + 1);

    const std::size_t batch_bytes =
        std::min({remaining, table_coverage - curr_virt.page_offset(table_coverage),
                  MAX_LOG_PTES * step_size});

    auto pt_res = walk(curr_virt, false, target_size);
    if (!pt_res) [[unlikely]] {
      return std::unexpected(pt_res.error());
    }

    UndoLogNode* log = guard.begin(curr_virt, target_size);
    if (!log) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    PageTableEntry* pte = *pt_res;
    for (std::size_t i = 0; i < batch_bytes / step_size; ++i) {
      PTEntry expected = pte[i].load(std::memory_order_acquire);

      if (!expected.get_present()) [[unlikely]] {
        return std::unexpected(Error::NotMapped);
      }

      // A huge batch ends at the first slot that points to a table instead; the next round
      // handles it at the smaller size.
      if (target_size != PageSize::Size4K && !expected.get_huge()) {
        break;
      }

      const PhysicalAddress offset_phys =
          curr_phys + static_cast<PhysicalAddress::OffsetType>(i * step_size);
      const PTEntry desired =
          PageTableEntry::build_schema(offset_phys, lflags, cache, target_size, pkey);

      log->original_ptes[log->count] = expected;
      while (!pte[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) {
        log->original_ptes[log->count] = expected;
      }

      log->count++;
    }

    const std::size_t done = log->count * step_size;
    curr_virt += static_cast<VirtualAddress::OffsetType>(done);
    curr_phys += static_cast<PhysicalAddress::OffsetType>(done);
    remaining -= done;
    guard.attempted_bytes += done;
  }

  guard.success = true;
  tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes);

  return {};
}

std::expected<void, Error> PageMap::protect_virtual_range(const VirtualAddress start_virt,
                                                          const std::size_t size_bytes,
                                                          const AccessFlags flags) noexcept {
  if (start_virt.is_upper_half() && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  if (!start_virt.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE))
      [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  RollbackGuard guard{*this, start_virt};

  const bool writable = has_flag(flags, AccessFlags::Write);
  const bool no_exec = g_paging_features.nx && !has_flag(flags, AccessFlags::Execute);
  const bool user = has_flag(flags, AccessFlags::User);

  const VirtualAddress end_virt = start_virt + static_cast<VirtualAddress::OffsetType>(size_bytes);
  VirtualAddress curr = start_virt;

  while (curr < end_virt) {
    const auto remaining = static_cast<std::size_t>(end_virt - curr);
    const auto [entry, lvl, value] = probe(curr);
    const std::size_t coverage = level_coverage(lvl);

    if (!value.get_present()) {
      curr += static_cast<VirtualAddress::OffsetType>(
          std::min(remaining, coverage - curr.page_offset(coverage)));
      continue;
    }

    // Changing part of a huge page means splitting it first.
    if (lvl > 1 && (!curr.is_aligned(coverage) || remaining < coverage)) {
      if (const auto res = walk(curr, false, static_cast<PageSize>(lvl - 1)); !res) [[unlikely]] {
        return std::unexpected(res.error());
      }

      continue;
    }

    // Leaves of this size from here to the end of their table (or of the range).
    const std::size_t table_coverage = level_coverage(lvl + 1);
    const std::size_t batch_pages =
        std::min({remaining, table_coverage - curr.page_offset(table_coverage),
                  MAX_LOG_PTES * coverage}) /
        coverage;

    UndoLogNode* log = guard.begin(curr, static_cast<PageSize>(lvl));
    if (!log) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    for (std::size_t i = 0; i < batch_pages; ++i) {
      PTEntry expected = entry[i].load(std::memory_order_acquire);

      // Slots holding a table end a huge batch; the next round goes down into them.
      if (lvl > 1 && expected.get_present() && !expected.get_huge()) {
        break;
      }

      log->original_ptes[log->count] = expected;

      while (expected.get_present()) {
        PTEntry desired = expected;
        // Write access on a copy-on-write page is granted by the fault handler, after the copy.
        desired.set_rw(writable && !expected.get_cow());
        desired.set_nx(no_exec);
        desired.set_user(user);

        if (desired.raw() == expected.raw() ||
            entry[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) {
          break;
        }

        log->original_ptes[log->count] = expected;
      }

      log->count++;
    }

    const std::size_t done = log->count * coverage;
    curr += static_cast<VirtualAddress::OffsetType>(done);
    guard.attempted_bytes = static_cast<std::size_t>(curr - start_virt);
  }

  guard.success = true;
  tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes);

  return {};
}

void PageMap::write_protect_range(const VirtualAddress start_virt,
                                  const std::size_t size_bytes) noexcept {
  const VirtualAddress end_virt = start_virt + static_cast<VirtualAddress::OffsetType>(size_bytes);
  VirtualAddress curr = start_virt;
  bool changed = false;

  const auto clear_rw = [&](PageTableEntry& pte) {
    PTEntry expected = pte.load(std::memory_order_acquire);
    while (expected.get_present() && expected.get_rw()) {
      if (pte.cas(expected, PTEntry{expected.raw()}.with_rw(false), std::memory_order_release,
                  std::memory_order_acquire)) {
        changed = true;
        return;
      }
    }
  };

  while (curr < end_virt) {
    const auto remaining = static_cast<std::size_t>(end_virt - curr);
    const auto [entry, lvl, value] = probe(curr);
    const std::size_t coverage = level_coverage(lvl);

    if (!value.get_present() || lvl > 1) {
      // A hole, or a huge page that is protected as a whole.
      if (value.get_present()) {
        clear_rw(*entry);
      }

      curr += static_cast<VirtualAddress::OffsetType>(
          std::min(remaining, coverage - curr.page_offset(coverage)));
      continue;
    }

    const std::size_t batch_bytes =
        std::min(remaining, PAGE_SIZE_2MB - curr.page_offset(PAGE_SIZE_2MB));
    for (std::size_t i = 0; i < batch_bytes / PAGE_SIZE; ++i) {
      clear_rw(entry[i]);
    }

    curr += static_cast<VirtualAddress::OffsetType>(batch_bytes);
  }

  if (changed) {
    tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes);
  }
}
}  // namespace kernel::memory::vmm
