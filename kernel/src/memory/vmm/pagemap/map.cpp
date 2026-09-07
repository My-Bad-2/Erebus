#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> PageMap::map_1gb_as_2mb(const VirtualAddress virt, const PhysicalAddress phys,
                                                   const AccessFlags flags, const CacheMode cache,
                                                   const std::uint8_t pkey) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB) || !phys.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, true, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  PageTableEntry *pdpte = *pdpte_res;
  while (true) {
    PteSchema pdpte_schema = pdpte->load(std::memory_order_acquire);

    // Case 1: The entire 1GB region's directory is unmapped. We create the Page Directory.
    if (pdpte_schema.small().get<"present">() == 0) [[likely]] {
      const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
      if (!new_page_res) [[unlikely]] {
        return std::unexpected(Error::OutOfMemory);
      }

      const PhysicalAddress new_phys{*new_page_res};
      pmm::phys_to_page(new_phys)->set_mobility(pmm::PageMobility::Unmovable);
      auto *pd_table = DirectMap::phys_to_virt(new_phys).as<PageTableEntry>();

      for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
        const PhysicalAddress offset_phys = phys + (i * PAGE_SIZE_2MB);

        PteSchema child_schema = PageTableEntry::build_schema(offset_phys, flags, cache, PageSize::Size2M, pkey);
        pd_table[i].store(child_schema, std::memory_order_relaxed);
      }

      PteLeafSchema dir_leaf{};
      dir_leaf.set_mut<"present">(1);
      dir_leaf.set_mut<"pfn">(new_phys.value() >> 12);
      dir_leaf.set_mut<"rw">(1);
      dir_leaf.set_mut<"user">(1);

      const PteSchema dir_schema{static_cast<std::uint64_t>(dir_leaf)};
      if (pdpte->cas(pdpte_schema, dir_schema, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
        return {};
      }

      pmm::free_pages(new_phys, 0);
      continue;
    }

    // Case 2: The region is already mapped as a single 1GB Huge Page
    if ((pdpte_schema.raw & (1ull << 7)) != 0) [[unlikely]] {
      return std::unexpected(Error::AlreadyMapped);
    }

    // Case 3: The directory already exists. We must populate its 512 entries.
    const PhysicalAddress pd_phys{pdpte_schema.small().get<"pfn">() << 12};
    auto *pd_table = DirectMap::phys_to_virt(pd_phys).as<PageTableEntry>();

    for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
      PteSchema expected = pd_table[i].load(std::memory_order_acquire);
      bool success = false;

      if (expected.small().get<"present">() == 0) [[likely]] {
        const PhysicalAddress offset_phys = phys + (i * PAGE_SIZE_2MB);
        PteSchema desired = PageTableEntry::build_schema(offset_phys, flags, cache, PageSize::Size2M, pkey);
        success = pd_table[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire);
      }

      if (!success) [[unlikely]] {
        for (std::uint32_t j = 0; j < i; ++j) {
          const VirtualAddress rollback_virt = virt + (j * PAGE_SIZE_2MB);
          [[maybe_unused]] auto _ = unmap(rollback_virt, PageSize::Size2M);
        }

        return std::unexpected(Error::AlreadyMapped);
      }
    }

    return {};
  }
}

std::expected<void, Error> PageMap::map(const VirtualAddress virt, const PhysicalAddress phys, const AccessFlags flags,
                                        const CacheMode cache, const std::uint8_t pkey, const PageSize size) noexcept {
  const bool is_kernel_addr = (virt.value() >= DirectMap::s_hhdm_base);
  if (is_kernel_addr && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  const std::size_t page_size_bytes = size_to_bytes(size);
  if (!virt.is_aligned(page_size_bytes) || !phys.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !supports_1gb_pages) [[unlikely]] {
    return map_1gb_as_2mb(virt, phys, flags, cache, pkey);
  }

  auto pte_res = walk(virt, true, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry *pte = *pte_res;

  while (true) {
    PteSchema expected = pte->load(std::memory_order_acquire);
    if (expected.small().get<"present">() == 1) [[unlikely]] {
      return std::unexpected(Error::AlreadyMapped);
    }

    const PteSchema desired = PageTableEntry::build_schema(phys, flags, cache, size, pkey);
    if (pte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
      return {}; // Successfully mapped
    }
  }
}

[[nodiscard]] std::expected<void, Error> PageMap::map_range(const VirtualAddress start_virt,
                                                            const PhysicalAddress start_phys,
                                                            const std::size_t size_bytes, const AccessFlags flags,
                                                            const CacheMode cache, const std::uint8_t pkey) noexcept {
  if (!start_virt.is_aligned(PAGE_SIZE) || !start_phys.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  VirtualAddress curr_virt = start_virt;
  PhysicalAddress curr_phys = start_phys;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    if (remaining >= PAGE_SIZE_1GB && curr_virt.is_aligned(PAGE_SIZE_1GB) && curr_phys.is_aligned(PAGE_SIZE_1GB)) {
      const auto res = map(curr_virt, curr_phys, flags, cache, pkey, PageSize::Size1G);
      if (!res) [[unlikely]] {
        [[maybe_unused]] auto _ = unmap_range(start_virt, size_bytes - remaining);
        return std::unexpected(res.error());
      }

      curr_virt += PAGE_SIZE_1GB;
      curr_phys += PAGE_SIZE_1GB;
      remaining -= PAGE_SIZE_1GB;
      continue;
    }

    if (remaining >= PAGE_SIZE_2MB && curr_virt.is_aligned(PAGE_SIZE_2MB) && curr_phys.is_aligned(PAGE_SIZE_2MB)) {
      const auto res = map(curr_virt, curr_phys, flags, cache, pkey, PageSize::Size2M);
      if (!res) [[unlikely]] {
        [[maybe_unused]] auto _ = unmap_range(start_virt, size_bytes - remaining);
        return std::unexpected(res.error());
      }

      curr_virt += PAGE_SIZE_2MB;
      curr_phys += PAGE_SIZE_2MB;
      remaining -= PAGE_SIZE_2MB;
      continue;
    }

    const std::size_t bytes_to_pml1_boundary = PAGE_SIZE_2MB - curr_virt.page_offset(PAGE_SIZE_2MB);
    const std::size_t batch_bytes = std::min(remaining, bytes_to_pml1_boundary);
    const std::size_t pages_in_batch = batch_bytes / PAGE_SIZE;

    const auto pt_res = walk(curr_virt, true, PageSize::Size4K);
    if (!pt_res) [[unlikely]] {
      [[maybe_unused]] auto _ = unmap_range(start_virt, size_bytes - remaining);
      return std::unexpected(pt_res.error());
    }

    PageTableEntry *pte = *pt_res;
    for (std::size_t i = 0; i < pages_in_batch; ++i) {
      while (true) {
        PteSchema expected = pte[i].load(std::memory_order_acquire);

        if (expected.small().get<"present">() == 1) [[unlikely]] {
          const std::size_t mapped_so_far = (size_bytes - remaining) + (i * PAGE_SIZE);
          [[maybe_unused]] auto _ = unmap_range(start_virt, mapped_so_far);
          return std::unexpected(Error::AlreadyMapped);
        }

        const PhysicalAddress offset_phys = curr_phys + (i * PAGE_SIZE);
        const PteSchema desired = PageTableEntry::build_schema(offset_phys, flags, cache, PageSize::Size4K, pkey);
        if (pte[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
          break;
        }
      }
    }

    curr_virt += batch_bytes;
    curr_phys += batch_bytes;
    remaining -= batch_bytes;
  }

  return {};
}
} // namespace kernel::memory::vmm