#pragma once

#include <atomic>
#include <cstdint>
#include <expected>

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
  std::uint8_t pkey;
};

class PageMap {
  friend struct RollbackGuard;
  PhysicalAddress m_root_phys;
  hw::ActivePcidTracker m_cpu_tracker;
  std::uint8_t m_lvls{0}; // 4 or 5

  static inline VirtualAddress s_kernel_root{};
  static constexpr std::uint32_t MAX_PAGE_ENTRIES = 512;

  static constexpr std::size_t size_to_bytes(const PageSize size) noexcept {
    return PAGE_SIZE << ((std::to_underlying(size) - 1) * 9);
  }

  static AccessFlags extract_flags(const PTEntry &schema) noexcept;
  static CacheMode extract_cache(const PTEntry &schema, bool is_huge) noexcept;

  std::expected<void, Error> shatter_huge_page(PageTableEntry *pte, VirtualAddress virt,
                                               std::uint8_t curr_lvl) noexcept;
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

  explicit PageMap() noexcept;
  ~PageMap() noexcept;

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
  [[nodiscard]] std::expected<void, Error> protect_virtual_range(VirtualAddress start_virt, std::size_t size_bytes,
                                                                 AccessFlags flags) noexcept;
  [[nodiscard]] std::expected<void, Error> alias_virtual_range(VirtualAddress src_virt, VirtualAddress dest_virt,
                                                               std::size_t size_bytes) noexcept;
  [[nodiscard]] std::expected<void, Error> move_virtual_range(VirtualAddress src_virt, VirtualAddress dest_virt,
                                                              std::size_t size_bytes) noexcept;
};

void early_initialize_hw() noexcept;
void initialize_hw() noexcept;
} // namespace kernel::memory::vmm