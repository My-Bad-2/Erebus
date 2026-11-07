#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"

namespace kernel::memory::vmm {
namespace {
[[nodiscard]] bool user_in_kernel_half(const VirtualAddress virt,
                                       const AccessFlags flags) noexcept {
  return virt.is_upper_half() && has_flag(flags, AccessFlags::User);
}
}  // namespace

std::expected<void, Error> PageMap::map_1gb_as_2mb(const VirtualAddress virt,
                                                   const PhysicalAddress phys,
                                                   const AccessFlags flags, const CacheMode cache,
                                                   const std::uint8_t pkey) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB) || !phys.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, true, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  const AccessFlags lflags = leaf_flags(virt, flags);
  const auto entry_for = [&](const std::uint32_t i) {
    const PhysicalAddress offset_phys =
        phys + static_cast<PhysicalAddress::OffsetType>(i * PAGE_SIZE_2MB);
    return PageTableEntry::build_schema(offset_phys, lflags, cache, PageSize::Size2M, pkey);
  };

  PageTableEntry* pdpte = *pdpte_res;
  while (true) {
    PTEntry pdpte_entry = pdpte->load(std::memory_order_acquire);

    // Nothing there yet: build a complete page directory and install it in one go.
    if (!pdpte_entry.get_present()) [[likely]] {
      const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
      if (!new_page_res) [[unlikely]] {
        return std::unexpected(Error::OutOfMemory);
      }

      auto* pd_table = DirectMap::phys_to_virt(*new_page_res).as<PageTableEntry>();
      for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
        pd_table[i].store(entry_for(i), std::memory_order_relaxed);
      }

      const PTEntry dir_schema =
          PageTableEntry::build_directory(*new_page_res, !virt.is_upper_half());
      if (pdpte->cas(pdpte_entry, dir_schema, std::memory_order_release, std::memory_order_acquire))
          [[likely]] {
        return {};
      }

      // Another CPU installed something first; look again.
      pmm::free_pages(*new_page_res, 0);
      continue;
    }

    if (pdpte_entry.get_huge()) [[unlikely]] {
      return std::unexpected(Error::AlreadyMapped);
    }

    // A directory already exists: fill all 512 slots, or none.
    PageTableEntry* pd_table =
        DirectMap::phys_to_virt(PhysicalAddress{pdpte_entry.get_pfn_4k() << PAGE_SHIFT_4KB})
            .as<PageTableEntry>();

    for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
      PTEntry expected = pd_table[i].load(std::memory_order_acquire);
      bool installed = false;

      while (!expected.get_present()) {
        if (pd_table[i].cas(expected, entry_for(i), std::memory_order_release,
                            std::memory_order_acquire)) [[likely]] {
          installed = true;
          break;
        }
      }

      if (!installed) [[unlikely]] {
        for (std::uint32_t j = 0; j < i; ++j) {
          auto _ = unmap(virt + static_cast<VirtualAddress::OffsetType>(j * PAGE_SIZE_2MB),
                         PageSize::Size2M);
        }

        return std::unexpected(Error::AlreadyMapped);
      }
    }

    return {};
  }
}

std::expected<void, Error> PageMap::map(const VirtualAddress virt, const PhysicalAddress phys,
                                        const AccessFlags flags, const CacheMode cache,
                                        const std::uint8_t pkey, const PageSize size) noexcept {
  if (user_in_kernel_half(virt, flags)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  const std::size_t page_size_bytes = size_to_bytes(size);
  if (!virt.is_aligned(page_size_bytes) || !phys.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !g_paging_features.huge_1g) [[unlikely]] {
    return map_1gb_as_2mb(virt, phys, flags, cache, pkey);
  }

  auto pte_res = walk(virt, true, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry* pte = *pte_res;
  const PTEntry desired =
      PageTableEntry::build_schema(phys, leaf_flags(virt, flags), cache, size, pkey);

  PTEntry expected = pte->load(std::memory_order_acquire);
  while (!expected.get_present()) {
    if (pte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire))
        [[likely]] {
      return {};
    }
  }

  return std::unexpected(Error::AlreadyMapped);
}

std::expected<void, Error> PageMap::map_range(const VirtualAddress start_virt,
                                              const PhysicalAddress start_phys,
                                              const std::size_t size_bytes, const AccessFlags flags,
                                              const CacheMode cache,
                                              const std::uint8_t pkey) noexcept {
  if (user_in_kernel_half(start_virt, flags)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  if (!start_virt.is_aligned(PAGE_SIZE) || !start_phys.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  const AccessFlags lflags = leaf_flags(start_virt, flags);

  VirtualAddress curr_virt = start_virt;
  PhysicalAddress curr_phys = start_phys;
  std::size_t remaining = size_bytes;

  const auto fail = [&](const Error err) -> std::expected<void, Error> {
    [[maybe_unused]] auto _ = unmap_range(start_virt, size_bytes - remaining);
    return std::unexpected(err);
  };

  const auto advance = [&](const std::size_t bytes) {
    curr_virt += static_cast<VirtualAddress::OffsetType>(bytes);
    curr_phys += static_cast<PhysicalAddress::OffsetType>(bytes);
    remaining -= bytes;
  };

  // The biggest page that both addresses are aligned for and that still fits.
  const auto best_size = [&]() {
    for (const PageSize size : {PageSize::Size1G, PageSize::Size2M}) {
      const std::size_t bytes = size_to_bytes(size);
      if (remaining >= bytes && curr_virt.is_aligned(bytes) && curr_phys.is_aligned(bytes)) {
        return size;
      }
    }

    return PageSize::Size4K;
  };

  while (remaining > 0) {
    if (const PageSize huge = best_size(); huge != PageSize::Size4K) {
      if (const auto res = map(curr_virt, curr_phys, flags, cache, pkey, huge); !res) [[unlikely]] {
        return fail(res.error());
      }

      advance(size_to_bytes(huge));
      continue;
    }

    // 4 KiB pages up to the end of the current page table: one walk for the whole batch.
    const std::size_t bytes_to_table_end = PAGE_SIZE_2MB - curr_virt.page_offset(PAGE_SIZE_2MB);
    const std::size_t batch_bytes = std::min(remaining, bytes_to_table_end);

    const auto pt_res = walk(curr_virt, true, PageSize::Size4K);
    if (!pt_res) [[unlikely]] {
      return fail(pt_res.error());
    }

    PageTableEntry* pte = *pt_res;
    for (std::size_t i = 0; i < batch_bytes / PAGE_SIZE; ++i) {
      const PhysicalAddress offset_phys =
          curr_phys + static_cast<PhysicalAddress::OffsetType>(i * PAGE_SIZE);
      const PTEntry desired =
          PageTableEntry::build_schema(offset_phys, lflags, cache, PageSize::Size4K, pkey);

      PTEntry expected = pte[i].load(std::memory_order_acquire);
      bool installed = false;
      while (!expected.get_present()) {
        if (pte[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire))
            [[likely]] {
          installed = true;
          break;
        }
      }

      if (!installed) [[unlikely]] {
        remaining -= i * PAGE_SIZE;  // so fail() also undoes this batch's pages
        return fail(Error::AlreadyMapped);
      }
    }

    advance(batch_bytes);
  }

  return {};
}
}  // namespace kernel::memory::vmm
