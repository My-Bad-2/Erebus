#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"

namespace kernel::memory::vmm {
std::expected<PhysicalAddress, Error> PageMap::unmap_1gb_as_2mb(const VirtualAddress virt) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, false, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  PageTableEntry *pdpte = *pdpte_res;
  PTEntry expected = pdpte->load(std::memory_order_acquire);

  while (true) {
    if (!expected.get_present()) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    if (expected.get_huge()) [[unlikely]] {
      return std::unexpected(Error::InvalidFlags);
    }

    const PTEntry desired;

    if (pdpte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
      tlb::ShootdownCoordinator::broadcast(this, virt, PAGE_SIZE_1GB, true);

      const PhysicalAddress pml2_phys{expected.get_pfn_4k() << PAGE_SHIFT_4KB};
      auto *pml2_table = DirectMap::phys_to_virt(pml2_phys).as<PageTableEntry>();

      PhysicalAddress user_base_phys{};
      const PTEntry first_pde = pml2_table[0].load(std::memory_order_acquire);
      if (first_pde.get_present()) [[likely]] {
        const PageTableEntry first_pte{first_pde.raw()};
        if (auto phys_res = first_pte.extract_address(PageSize::Size2M)) [[likely]] {
          user_base_phys = *phys_res;
        }
      }

      pmm::free_pages(pml2_phys, 0);
      return user_base_phys;
    }
  }
}

std::expected<PhysicalAddress, Error> PageMap::unmap(const VirtualAddress virt, const PageSize size) noexcept {
  const std::size_t page_size_bytes = size_to_bytes(size);
  if (!virt.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !supports_1gb_pages) [[unlikely]] {
    return unmap_1gb_as_2mb(virt);
  }

  auto pte_res = walk(virt, false, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry *pte = *pte_res;
  PTEntry expected = pte->load(std::memory_order_acquire);

  while (true) {
    if (expected.get_present() == 0) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    const PTEntry desired{0};
    if (pte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
      const PageTableEntry old_pte{expected.raw()};

      auto addr_res = old_pte.extract_address(size);
      if (!addr_res) [[unlikely]] {
        return std::unexpected(Error::NotMapped);
      }

      const bool is_huge = (size != PageSize::Size4K);
      tlb::ShootdownCoordinator::broadcast(this, virt, page_size_bytes, is_huge);

      return *addr_res;
    }
  }
}

std::expected<void, Error> PageMap::unmap_range(const VirtualAddress start_virt,
                                                const std::size_t size_bytes) noexcept {
  if (!start_virt.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  VirtualAddress curr_virt = start_virt;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    const auto trans = translate(curr_virt);

    // If not mapped, fast-forward by one page.
    if (!trans) {
      curr_virt += PAGE_SIZE;
      remaining -= PAGE_SIZE;
      continue;
    }

    if (trans->mapped_size == PageSize::Size1G && remaining >= PAGE_SIZE_1GB && curr_virt.is_aligned(PAGE_SIZE_1GB)) {
      [[maybe_unused]] auto _ = unmap(curr_virt, PageSize::Size1G);
      curr_virt += PAGE_SIZE_1GB;
      remaining -= PAGE_SIZE_1GB;
      continue;
    }

    if (trans->mapped_size == PageSize::Size2M && remaining >= PAGE_SIZE_2MB && curr_virt.is_aligned(PAGE_SIZE_2MB)) {
      [[maybe_unused]] auto _ = unmap(curr_virt, PageSize::Size2M);
      curr_virt += PAGE_SIZE_2MB;
      remaining -= PAGE_SIZE_2MB;
      continue;
    }

    const std::size_t bytes_to_pml1_boundary = PAGE_SIZE_2MB - curr_virt.page_offset(PAGE_SIZE_2MB);
    const std::size_t batch_bytes = std::min(remaining, bytes_to_pml1_boundary);
    const std::size_t pages_in_batch = batch_bytes / PAGE_SIZE;

    const auto pt_res = walk(curr_virt, false, PageSize::Size4K);
    if (pt_res) [[likely]] {
      PageTableEntry *pte = *pt_res;

      for (std::size_t i = 0; i < pages_in_batch; ++i) {
        PTEntry expected = pte[i].load(std::memory_order_acquire);
        while (expected.get_present()) {
          if (pte[i].cas(expected, PTEntry{}, std::memory_order_release, std::memory_order_acquire)) {
            break;
          }
        }
      }
    }

    curr_virt += batch_bytes;
    remaining -= batch_bytes;
  }

  tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes, size_bytes >= PAGE_SIZE_2MB);
  return {};
}
} // namespace kernel::memory::vmm