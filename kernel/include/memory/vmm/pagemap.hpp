#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <type_traits>

#include "active_pcid.hpp"
#include "memory/memory.hpp"
#include "pte.hpp"

namespace kernel::memory::vmm {
inline bool supports_1gb_pages{false};

struct TranslationResult {
  PhysicalAddress phys_address;
  AccessFlags flags;
  CacheMode cache;
  PageSize mapped_size;
};

class PageMap {
  PhysicalAddress m_root_phys;
  hw::ActivePcidTracker m_cpu_tracker;
  std::uint8_t m_lvls{0}; // 4 or 5

  static constexpr std::uint32_t MAX_PAGE_ENTRIES = 512;

  explicit PageMap(const PhysicalAddress root, const std::uint8_t lvls) noexcept : m_root_phys{root}, m_lvls{lvls} {}

  static constexpr int size_to_level(const PageSize size) noexcept {
    switch (size) {
    case PageSize::Size1G:
      return 3; // PDPT
    case PageSize::Size2M:
      return 2; // PD
    case PageSize::Size4K:
    default:
      return 1; // PT
    }
  }

  static constexpr std::size_t size_to_bytes(const PageSize size) noexcept {
    switch (size) {
    case PageSize::Size1G:
      return PAGE_SIZE_1GB;
    case PageSize::Size2M:
      return PAGE_SIZE_2MB;
    default:
    case PageSize::Size4K:
      return PAGE_SIZE;
    }
  }

  static AccessFlags extract_flags(const PteSchema &schema) noexcept;
  static CacheMode extract_cache(const PteSchema &schema, bool is_huge) noexcept;

  std::expected<void, Error> shatter_huge_page(PageTableEntry *pte, VirtualAddress virt,
                                               std::uint8_t curr_lvl) const noexcept;
  [[nodiscard]] std::expected<PageTableEntry *, Error> walk(VirtualAddress virt, bool alloc,
                                                            PageSize target_size) noexcept;

  [[nodiscard]] std::expected<void, Error> map_1gb_as_2mb(VirtualAddress virt, PhysicalAddress phys, AccessFlags flags,
                                                          CacheMode cache, std::uint8_t pkey) noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> unmap_1gb_as_2mb(VirtualAddress virt) noexcept;
  [[nodiscard]] std::expected<void, Error> remap_1gb_as_2mb(VirtualAddress virt, PhysicalAddress new_phys,
                                                            AccessFlags flags, CacheMode cache,
                                                            std::uint8_t pkey) noexcept;

public:
  [[nodiscard]] PhysicalAddress root_phys() const noexcept { return m_root_phys; }
  [[nodiscard]] std::uint64_t cr3_base() const noexcept { return m_root_phys.value(); }

  [[nodiscard]] hw::ActivePcidTracker &cpu_tracker() noexcept { return m_cpu_tracker; }

  [[nodiscard]] static std::expected<PageMap *, Error> create() noexcept;
  static void destroy(PageMap *map) noexcept;

  [[nodiscard]] std::expected<void, Error> map(VirtualAddress virt, PhysicalAddress phys, AccessFlags flags,
                                               CacheMode cache = CacheMode::WriteBack, std::uint8_t pkey = 0,
                                               PageSize size = PageSize::Size4K) noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> unmap(VirtualAddress virt,
                                                            PageSize size = PageSize::Size4K) noexcept;
  [[nodiscard]] std::expected<void, Error> remap(VirtualAddress virt, PhysicalAddress new_phys, AccessFlags flags,
                                                 CacheMode cache = CacheMode::WriteBack, std::uint8_t pkey = 0,
                                                 PageSize size = PageSize::Size4K) noexcept;
  [[nodiscard]] std::optional<TranslationResult> translate(VirtualAddress virt) const noexcept;

  [[nodiscard]] std::expected<void, Error> map_range(VirtualAddress start_virt, PhysicalAddress start_phys,
                                                     std::size_t size_bytes, AccessFlags flags,
                                                     CacheMode cache = CacheMode::WriteBack,
                                                     std::uint8_t pkey = 0) noexcept;
  [[nodiscard]] std::expected<void, Error> unmap_range(VirtualAddress start_virt, std::size_t size_bytes) noexcept;
  [[nodiscard]] std::expected<void, Error> remap_range(VirtualAddress start_virt, PhysicalAddress start_phys,
                                                       std::size_t size_bytes, AccessFlags flags,
                                                       CacheMode cache = CacheMode::WriteBack,
                                                       std::uint8_t pkey = 0) noexcept;
};

const PageMap *get_kernel_pagemap() noexcept;
void early_initialize_hw() noexcept;
void initialize_hw() noexcept;
} // namespace kernel::memory::vmm