#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "memory/vmm/pagemap/tlb.hpp"

namespace kernel::memory::vmm {
namespace {
[[nodiscard]] constexpr std::uint32_t table_index(const VirtualAddress virt,
                                                  const std::uint8_t lvl) noexcept {
  return (virt.value() >> (PAGE_SHIFT_4KB + 9 * (lvl - 1))) & 0x1FF;
}

[[nodiscard]] PageTableEntry* table_at(const PhysicalAddress phys) noexcept {
  return DirectMap::phys_to_virt(phys).as<PageTableEntry>();
}

[[nodiscard]] PageTableEntry* next_table(const PTEntry directory) noexcept {
  return table_at(PhysicalAddress{directory.get_pfn_4k() << PAGE_SHIFT_4KB});
}
}  // namespace

std::expected<void, Error> PageMap::shatter_huge_page(PageTableEntry* pte,
                                                      const VirtualAddress virt,
                                                      const std::uint8_t curr_lvl) noexcept {
  PTEntry old_schema = pte->load(std::memory_order_acquire);
  if (!old_schema.get_present() || !old_schema.get_huge()) {
    return {};  // Already shattered or not mapped, nothing to do
  }

  // Only 1 GiB (level 3) and 2 MiB (level 2) leaves exist.
  if (curr_lvl != 3 && curr_lvl != 2) [[unlikely]] {
    return std::unexpected(Error::InvalidFlags);
  }

  const PageSize curr_size = (curr_lvl == 3) ? PageSize::Size1G : PageSize::Size2M;
  const PageSize next_size = (curr_lvl == 3) ? PageSize::Size2M : PageSize::Size4K;
  const std::size_t step_bytes = size_to_bytes(next_size);

  const auto base_phys = PageTableEntry{old_schema.raw()}.extract_address(curr_size);
  if (!base_phys) {
    return std::unexpected(Error::NotMapped);
  }

  const AccessFlags flags = extract_flags(old_schema);
  const CacheMode cache = extract_cache(old_schema, true);
  const std::uint8_t pkey = old_schema.get_pkey();

  const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
  if (!new_page_res) {
    return std::unexpected(Error::OutOfMemory);
  }

  const PhysicalAddress new_phys = *new_page_res;
  PageTableEntry* new_table = table_at(new_phys);

  for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
    const PhysicalAddress offset_phys =
        *base_phys + static_cast<PhysicalAddress::OffsetType>(i * step_bytes);
    PTEntry child = PageTableEntry::build_schema(offset_phys, flags, cache, next_size, pkey);

    // Carry the hardware-maintained bits over, or writes to the huge page would be forgotten.
    child.set_accessed(old_schema.get_accessed());
    child.set_dirty(old_schema.get_dirty());
    new_table[i].store(child, std::memory_order_relaxed);
  }

  const PTEntry dir_schema = PageTableEntry::build_directory(new_phys, !virt.is_upper_half());
  if (pte->cas(old_schema, dir_schema, std::memory_order_release, std::memory_order_acquire))
      [[likely]] {
    // The huge translation and the new small ones must not coexist in the TLB.
    const std::size_t size = size_to_bytes(curr_size);
    tlb::ShootdownCoordinator::broadcast(this, virt.align_down(size), size, curr_size);
    return {};
  }

  // Someone changed the entry first, most likely by shattering it too. Drop our copy and let the
  // caller look at the entry again.
  pmm::free_pages(new_phys);
  return {};
}

std::expected<PageTableEntry*, Error> PageMap::walk(const VirtualAddress virt, const bool alloc,
                                                    const PageSize target_size) noexcept {
  PageTableEntry* curr_table = table_at(m_root_phys);
  const std::uint8_t target_lvl = std::to_underlying(target_size);
  const bool user = !virt.is_upper_half();

  for (std::uint8_t lvl = m_lvls; lvl > target_lvl; --lvl) {
    PageTableEntry* pte = &curr_table[table_index(virt, lvl)];

    while (true) {
      PTEntry schema = pte->load(std::memory_order_acquire);

      if (!schema.get_present()) [[unlikely]] {
        if (!alloc) {
          return std::unexpected(Error::NotMapped);
        }

        const auto new_phys_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
        if (!new_phys_res) {
          return std::unexpected(Error::OutOfMemory);
        }

        const PTEntry dir_schema = PageTableEntry::build_directory(*new_phys_res, user);
        if (pte->cas(schema, dir_schema, std::memory_order_release, std::memory_order_acquire))
            [[likely]] {
          curr_table = table_at(*new_phys_res);
          break;
        }

        // Another CPU installed a table first; use theirs.
        pmm::free_pages(*new_phys_res, 0);
        continue;
      }

      // A huge leaf above the target level has to be split before we can go deeper.
      if (schema.get_huge()) [[unlikely]] {
        if (const auto shatter_res = shatter_huge_page(pte, virt, lvl); !shatter_res) {
          return std::unexpected(shatter_res.error());
        }

        continue;
      }

      curr_table = next_table(schema);
      break;
    }
  }

  return &curr_table[table_index(virt, target_lvl)];
}

PageMap::Probe PageMap::probe(const VirtualAddress virt) const noexcept {
  PageTableEntry* table = table_at(m_root_phys);

  for (std::uint8_t lvl = m_lvls;; --lvl) {
    PageTableEntry* pte = &table[table_index(virt, lvl)];
    const PTEntry value = pte->load(std::memory_order_acquire);

    // The PS bit only means "leaf" in PDEs and PDPTEs; it is reserved above that.
    const bool leaf = (lvl == 1) || (lvl <= 3 && value.get_huge());
    if (!value.get_present() || leaf) {
      return {.entry = pte, .level = lvl, .value = value};
    }

    table = next_table(value);
  }
}

std::optional<TranslationResult> PageMap::translate(const VirtualAddress virt) const noexcept {
  const auto [entry, lvl, value] = probe(virt);
  if (!value.get_present()) {
    return std::nullopt;
  }

  // probe() only stops at a present entry when it is a leaf, i.e. at levels 1-3.
  const auto mapped_size = static_cast<PageSize>(lvl);
  const auto base_phys = PageTableEntry{value.raw()}.extract_address(mapped_size);
  if (!base_phys) [[unlikely]] {
    return std::nullopt;
  }

  return TranslationResult{
      .phys_address = *base_phys + static_cast<PhysicalAddress::OffsetType>(
                                       virt.page_offset(size_to_bytes(mapped_size))),
      .flags = extract_flags(value),
      .cache = extract_cache(value, lvl > 1),
      .mapped_size = mapped_size,
      .pkey = value.get_pkey(),
  };
}
}  // namespace kernel::memory::vmm
