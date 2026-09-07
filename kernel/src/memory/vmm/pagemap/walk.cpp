#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> PageMap::shatter_huge_page(PageTableEntry *pte, VirtualAddress virt,
                                                      const std::uint8_t curr_lvl) const noexcept {
  PteSchema old_schema = pte->load(std::memory_order_acquire);
  if (!pte->is_present() || !pte->is_huge()) {
    return {}; // Shattered or not mapped
  }

  // Only shatter 1gb or 2mb pages
  if (curr_lvl != 3 && curr_lvl != 2) [[unlikely]] {
    return std::unexpected(Error::InvalidFlags);
  }

  const PageSize curr_size = (curr_lvl == 3) ? PageSize::Size1G : PageSize::Size2M;
  const PageSize next_size = (curr_lvl == 3) ? PageSize::Size2M : PageSize::Size4K;

  const std::size_t step_bytes = (curr_lvl == 3) ? PAGE_SIZE_2MB : PAGE_SIZE;

  const auto base_phys_res = pte->extract_address(curr_size);
  if (!base_phys_res) {
    return std::unexpected(Error::NotMapped);
  }

  const PhysicalAddress base_phys = *base_phys_res;
  const AccessFlags flags = extract_flags(old_schema);
  const CacheMode cache = extract_cache(old_schema, true);
  const std::uint8_t pkey = static_cast<std::uint8_t>(old_schema.small().get<"pkey">());

  const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
  if (!new_page_res) {
    return std::unexpected(Error::OutOfMemory);
  }

  const PhysicalAddress new_phys = *new_page_res;
  pmm::phys_to_page(new_phys)->set_mobility(pmm::PageMobility::Unmovable);
  auto *new_table = DirectMap::phys_to_virt(new_phys).as<PageTableEntry>();

  for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
    const PhysicalAddress offset_phys = base_phys + (i * step_bytes);
    const PteSchema child_schema = PageTableEntry::build_schema(offset_phys, flags, cache, next_size, pkey);
    new_table[i].store(child_schema, std::memory_order_relaxed);
  }

  // Build the new directory entry
  PteLeafSchema dir_leaf{};
  dir_leaf.set_mut<"present">(1);
  dir_leaf.set_mut<"pfn">(new_phys.value() >> 12);
  dir_leaf.set_mut<"rw">(1);
  dir_leaf.set_mut<"user">(1);

  PteSchema dir_schema{static_cast<std::uint64_t>(dir_leaf)};
  if (pte->cas(old_schema, dir_schema, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
    const std::size_t size = size_to_bytes(curr_size);
    tlb::ShootdownCoordinator::broadcast(const_cast<PageMap *>(this), virt.align_down(size), true);
    return {};
  }

  pmm::free_pages(new_phys);
  return std::unexpected(Error::AlreadyMapped);
}

std::expected<PageTableEntry *, Error> PageMap::walk(const VirtualAddress virt, const bool alloc,
                                                     const PageSize target_size) noexcept {
  const std::uint16_t indices[] = {
      0,
      static_cast<std::uint16_t>(virt.pml1_index()),
      static_cast<std::uint16_t>(virt.pml2_index()),
      static_cast<std::uint16_t>(virt.pml3_index()),
      static_cast<std::uint16_t>(virt.pml4_index()),
      static_cast<std::uint16_t>(virt.pml5_index()),
  };

  auto *curr_table = DirectMap::phys_to_virt(m_root_phys).as<PageTableEntry>();
  const std::uint8_t target_lvl = std::to_underlying(target_size);

  for (int lvl = m_lvls; lvl > target_lvl; --lvl) {
    PageTableEntry *pte = &curr_table[indices[lvl]];

    while (true) {
      PteSchema schema = pte->load(std::memory_order_acquire);

      if (schema.small().get<"present">() == 0) [[unlikely]] {
        if (!alloc) {
          return std::unexpected(Error::NotMapped);
        }

        auto new_phys_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
        if (!new_phys_res) {
          return std::unexpected(Error::OutOfMemory);
        }

        PhysicalAddress new_phys{*new_phys_res};
        pmm::phys_to_page(new_phys)->set_mobility(pmm::PageMobility::Unmovable);

        PteLeafSchema dir_leaf{};
        dir_leaf.set_mut<"present">(1);
        dir_leaf.set_mut<"pfn">(new_phys.value() >> 12);
        dir_leaf.set_mut<"rw">(1);
        dir_leaf.set_mut<"user">(1);

        const PteSchema dir_schema{static_cast<std::uint64_t>(dir_leaf)};
        if (pte->cas(schema, dir_schema, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
          curr_table = DirectMap::phys_to_virt(new_phys).as<PageTableEntry>();
          break;
        }

        pmm::free_pages(new_phys);
        continue;
      }

      if ((schema.raw & (1ull << 7)) != 0 && lvl > 1) [[unlikely]] {
        const auto shatter_res = shatter_huge_page(pte, virt, lvl);
        if (!shatter_res) [[unlikely]] {
          return std::unexpected(shatter_res.error());
        }

        continue;
      }

      const PhysicalAddress next_phys{schema.small().get<"pfn">() << 12};
      curr_table = DirectMap::phys_to_virt(next_phys).as<PageTableEntry>();
      break;
    }
  }

  return &curr_table[indices[target_lvl]];
}

std::optional<TranslationResult> PageMap::translate(const VirtualAddress virt) const noexcept {
  const std::uint16_t indices[] = {
      0,
      static_cast<std::uint16_t>(virt.pml1_index()),
      static_cast<std::uint16_t>(virt.pml2_index()),
      static_cast<std::uint16_t>(virt.pml3_index()),
      static_cast<std::uint16_t>(virt.pml4_index()),
      static_cast<std::uint16_t>(virt.pml5_index()),
  };

  const auto *current_table = DirectMap::phys_to_virt(m_root_phys).as<PageTableEntry>();

  for (std::uint8_t lvl = m_lvls; lvl >= 1; --lvl) {
    const PageTableEntry pte = current_table[indices[lvl]];
    const PteSchema schema = pte.load(std::memory_order_acquire);
    if (schema.small().get<"present">() == 0) [[unlikely]] {
      return std::nullopt;
    }

    const bool is_huge = (lvl > 1) && ((schema.raw & (1ull << 7)) != 0);
    if (lvl == 1 || is_huge) [[unlikely]] {
      TranslationResult result{};

      if (lvl == 3) {
        result.mapped_size = PageSize::Size1G;
      } else if (lvl == 2) {
        result.mapped_size = PageSize::Size2M;
      } else {
        result.mapped_size = PageSize::Size4K;
      }

      const PageTableEntry leaf_pte{schema.raw};
      const auto base_phys_res = leaf_pte.extract_address(result.mapped_size);

      if (!base_phys_res) [[unlikely]] {
        return std::nullopt;
      }

      const PhysicalAddress base_phys = *base_phys_res;
      const std::size_t page_size_bytes = size_to_bytes(result.mapped_size);

      result.phys_address = base_phys + virt.page_offset(page_size_bytes);
      result.flags = extract_flags(schema);
      result.cache = extract_cache(schema, is_huge);
      result.pkey = schema.small().get<"pkey">();

      return result;
    }

    const PhysicalAddress next_table_phys{schema.small().get<"pfn">() << 12};
    current_table = DirectMap::phys_to_virt(next_table_phys).as<PageTableEntry>();
  }

  return std::nullopt;
}
} // namespace kernel::memory::vmm