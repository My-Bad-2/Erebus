#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "memory/vmm/pagemap/tlb.hpp"

namespace kernel::memory::vmm {
std::expected<PhysicalAddress, Error> PageMap::unmap_1gb_as_2mb(
    const VirtualAddress virt) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, false, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  PageTableEntry* pdpte = *pdpte_res;
  PTEntry expected = pdpte->load(std::memory_order_acquire);

  while (true) {
    if (!expected.get_present()) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    if (expected.get_huge()) [[unlikely]] {
      return std::unexpected(Error::InvalidFlags);
    }

    if (pdpte->cas(expected, PTEntry{}, std::memory_order_release, std::memory_order_acquire))
        [[likely]] {
      break;
    }
  }

  // Flush before the directory is freed, or a CPU could keep walking it from its
  // paging-structure caches.
  tlb::ShootdownCoordinator::broadcast(this, virt, PAGE_SIZE_1GB, PageSize::Size2M);

  const PhysicalAddress pd_phys{expected.get_pfn_4k() << PAGE_SHIFT_4KB};
  const auto* pd_table = DirectMap::phys_to_virt(pd_phys).as<PageTableEntry>();

  PhysicalAddress base_phys{};
  if (const auto first = pd_table[0].extract_address(PageSize::Size2M);
      first && pd_table[0].is_present()) [[likely]] {
    base_phys = *first;
  }

  pmm::free_pages(pd_phys, 0);
  return base_phys;
}

std::expected<PhysicalAddress, Error> PageMap::unmap(const VirtualAddress virt,
                                                     const PageSize size) noexcept {
  const std::size_t page_size_bytes = size_to_bytes(size);
  if (!virt.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !g_paging_features.huge_1g) [[unlikely]] {
    return unmap_1gb_as_2mb(virt);
  }

  auto pte_res = walk(virt, false, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry* pte = *pte_res;
  PTEntry expected = pte->load(std::memory_order_acquire);

  while (true) {
    if (!expected.get_present()) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    if (pte->cas(expected, PTEntry{}, std::memory_order_release, std::memory_order_acquire))
        [[likely]] {
      break;
    }
  }

  tlb::ShootdownCoordinator::broadcast(this, virt, page_size_bytes, size);

  const auto addr_res = PageTableEntry{expected.raw()}.extract_address(size);
  if (!addr_res) [[unlikely]] {
    return std::unexpected(Error::NotMapped);
  }

  return *addr_res;
}

std::expected<void, Error> PageMap::unmap_range(const VirtualAddress start_virt,
                                                const std::size_t size_bytes) noexcept {
  if (!start_virt.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE))
      [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  const VirtualAddress end_virt = start_virt + static_cast<VirtualAddress::OffsetType>(size_bytes);
  VirtualAddress curr = start_virt;
  bool cleared_any = false;

  while (curr < end_virt) {
    const auto remaining = static_cast<std::size_t>(end_virt - curr);
    const auto [entry, lvl, value] = probe(curr);
    const std::size_t coverage = level_coverage(lvl);

    // Nothing is mapped anywhere under a missing entry: skip all of it.
    if (!value.get_present()) {
      curr += static_cast<VirtualAddress::OffsetType>(
          std::min(remaining, coverage - curr.page_offset(coverage)));
      continue;
    }

    if (lvl > 1) {
      // A huge page. Drop it whole if the range covers it, otherwise split it and look again.
      if (curr.is_aligned(coverage) && remaining >= coverage) {
        PTEntry expected = value;
        if (entry->cas(expected, PTEntry{}, std::memory_order_release, std::memory_order_acquire)) {
          cleared_any = true;
          curr += static_cast<VirtualAddress::OffsetType>(coverage);
        }

        continue;
      }

      if (const auto res = walk(curr, false, static_cast<PageSize>(lvl - 1)); !res) [[unlikely]] {
        return std::unexpected(res.error());
      }

      continue;
    }

    // A page table: clear every slot of it that lies inside the range.
    const std::size_t batch_bytes =
        std::min(remaining, PAGE_SIZE_2MB - curr.page_offset(PAGE_SIZE_2MB));

    for (std::size_t i = 0; i < batch_bytes / PAGE_SIZE; ++i) {
      PTEntry expected = entry[i].load(std::memory_order_acquire);
      while (expected.get_present()) {
        if (entry[i].cas(expected, PTEntry{}, std::memory_order_release,
                         std::memory_order_acquire)) {
          cleared_any = true;
          break;
        }
      }
    }

    curr += static_cast<VirtualAddress::OffsetType>(batch_bytes);
  }

  // Huge pages are covered too: a 4 KiB stride over them is just more (or one full) flush.
  if (cleared_any) {
    tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes);
  }

  return {};
}
}  // namespace kernel::memory::vmm
