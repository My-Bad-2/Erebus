/// @file pagemap.hpp
/// @brief A hardware page table hierarchy (one CR3 worth of translations).

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>

#include "active_pcid.hpp"
#include "features.hpp"
#include "memory/memory.hpp"
#include "pte.hpp"

namespace kernel::memory::vmm {
/// @brief Result of PageMap::translate().
struct TranslationResult {
  PhysicalAddress phys_address;  ///< Physical address of the byte that was looked up.
  AccessFlags flags;
  CacheMode cache;
  PageSize mapped_size;  ///< Size of the leaf that maps it.
  std::uint8_t pkey;
};

/// @brief One x86-64 page table tree, 4 or 5 levels deep.
///
/// **Kernel half.** The first PageMap ever created becomes the kernel map. Its upper half
/// (top-level slots 256-511) is fully populated with empty tables up front, and every later
/// PageMap copies those 256 slots. All address spaces therefore share the very same kernel page
/// tables: a kernel mapping made through any of them is instantly visible in all, and only the
/// lower half is private. Kernel leaves are always created global, user leaves never are.
///
/// **Concurrency.** Entries are only changed with atomic CAS, and every operation re-reads an
/// entry after losing a race instead of assuming its old value. Intermediate tables are never
/// freed while the map is alive, so a concurrent walker can never step into a freed table. The
/// VMA layer is still expected to serialize conflicting operations on the same range.
///
/// **TLB.** Every operation that removes or downgrades a translation invalidates the TLBs before
/// it returns, through tlb::ShootdownCoordinator.
///
/// **Huge pages.** Ranges are mapped with the largest page size alignment allows. Operations on
/// part of a huge page split ("shatter") it into the next smaller size first. Without hardware
/// 1 GiB support, 1 GiB requests are emulated with a full table of 2 MiB pages.
class PageMap {
  friend struct RollbackGuard;

  PhysicalAddress m_root_phys;
  hw::ActivePcidTracker m_cpu_tracker;
  std::uint64_t m_generation;
  std::uint8_t m_lvls{0};

  static inline PhysicalAddress s_kernel_root{};
  static inline PageMap* s_kernel_map{nullptr};
  static inline std::atomic<std::uint64_t> s_next_generation{1};

  static constexpr std::uint32_t MAX_PAGE_ENTRIES = 512;
  static constexpr std::uint32_t KERNEL_HALF_START = MAX_PAGE_ENTRIES / 2;

  static constexpr std::size_t size_to_bytes(const PageSize size) noexcept {
    return PAGE_SIZE << ((std::to_underlying(size) - 1) * 9);
  }

  /// @brief Bytes of address space translated by one entry at paging level @p lvl.
  static constexpr std::size_t level_coverage(const std::uint8_t lvl) noexcept {
    return PAGE_SIZE << ((lvl - 1) * 9);
  }

  /// @brief Where a non-allocating walk stopped: at a leaf, or at the first non-present entry.
  struct Probe {
    PageTableEntry* entry;
    std::uint8_t level;
    PTEntry value;
  };

  static AccessFlags extract_flags(const PTEntry& schema) noexcept;
  static CacheMode extract_cache(const PTEntry& schema, bool is_huge) noexcept;

  /// @brief Leaf flags with the Global bit forced to match the half the address lives in.
  [[nodiscard]] static AccessFlags leaf_flags(VirtualAddress virt, AccessFlags flags) noexcept;

  [[nodiscard]] Probe probe(VirtualAddress virt) const noexcept;

  std::expected<void, Error> shatter_huge_page(PageTableEntry* pte, VirtualAddress virt,
                                               std::uint8_t curr_lvl) noexcept;
  [[nodiscard]] std::expected<PageTableEntry*, Error> walk(VirtualAddress virt, bool alloc,
                                                           PageSize target_size) noexcept;

  [[nodiscard]] std::expected<void, Error> map_1gb_as_2mb(VirtualAddress virt, PhysicalAddress phys,
                                                          AccessFlags flags, CacheMode cache,
                                                          std::uint8_t pkey) noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> unmap_1gb_as_2mb(
      VirtualAddress virt) noexcept;
  [[nodiscard]] std::expected<void, Error> remap_1gb_as_2mb(VirtualAddress virt,
                                                            PhysicalAddress new_phys,
                                                            AccessFlags flags, CacheMode cache,
                                                            std::uint8_t pkey) noexcept;

 public:
  [[nodiscard]] PhysicalAddress root_phys() const noexcept { return m_root_phys; }
  [[nodiscard]] std::uint64_t cr3_base() const noexcept { return m_root_phys.value(); }

  /// @brief CPUs that may hold TLB entries for this map (a superset, never cleared eagerly).
  [[nodiscard]] hw::ActivePcidTracker& cpu_tracker() noexcept { return m_cpu_tracker; }
  [[nodiscard]] const hw::ActivePcidTracker& cpu_tracker() const noexcept { return m_cpu_tracker; }

  /// @brief Identifier that is never reused, unlike the object's address.
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

  [[nodiscard]] bool is_kernel() const noexcept { return m_root_phys == s_kernel_root; }

  /// @brief The kernel map, which every CPU can go back to whatever it ran before.
  [[nodiscard]] static PageMap* kernel() noexcept { return s_kernel_map; }

  explicit PageMap() noexcept;

  /// @brief Frees the private (lower half) page tables. The frames they map are not touched.
  ///
  /// CPUs that still have the map loaded are moved to the kernel's first, which is an IPI to
  /// every CPU that ever loaded it (see tlb::ShootdownCoordinator::retire()).
  /// @pre The calling thread can wait for other CPUs.
  ~PageMap() noexcept;

  PageMap(const PageMap&) = delete;
  PageMap& operator=(const PageMap&) = delete;

  /// @brief Maps one page of @p size at @p virt.
  /// @return AlreadyMapped if the slot is in use, SecurityViolation for User in the upper half.
  [[nodiscard]] std::expected<void, Error> map(VirtualAddress virt, PhysicalAddress phys,
                                               AccessFlags flags,
                                               CacheMode cache = CacheMode::WriteBack,
                                               std::uint8_t pkey = 0,
                                               PageSize size = PageSize::Size4K) noexcept;

  /// @brief Removes the page of @p size at @p virt and flushes it from the TLBs.
  /// @return The physical address the page mapped.
  [[nodiscard]] std::expected<PhysicalAddress, Error> unmap(
      VirtualAddress virt, PageSize size = PageSize::Size4K) noexcept;

  /// @brief Points an existing mapping at @p new_phys with new attributes.
  [[nodiscard]] std::expected<void, Error> remap(VirtualAddress virt, PhysicalAddress new_phys,
                                                 AccessFlags flags,
                                                 CacheMode cache = CacheMode::WriteBack,
                                                 std::uint8_t pkey = 0,
                                                 PageSize size = PageSize::Size4K) noexcept;

  [[nodiscard]] std::optional<TranslationResult> translate(VirtualAddress virt) const noexcept;

  /// @brief Maps a physically contiguous range, using huge pages where alignment allows.
  ///
  /// All or nothing: if any page in the range is already mapped, everything mapped so far is
  /// undone and AlreadyMapped is returned.
  [[nodiscard]] std::expected<void, Error> map_range(VirtualAddress start_virt,
                                                     PhysicalAddress start_phys,
                                                     std::size_t size_bytes, AccessFlags flags,
                                                     CacheMode cache = CacheMode::WriteBack,
                                                     std::uint8_t pkey = 0) noexcept;

  /// @brief Unmaps whatever is mapped in the range. Holes are skipped a whole table at a time.
  [[nodiscard]] std::expected<void, Error> unmap_range(VirtualAddress start_virt,
                                                       std::size_t size_bytes) noexcept;

  /// @brief Re-points every page of an already fully mapped range. All or nothing.
  [[nodiscard]] std::expected<void, Error> remap_range(VirtualAddress start_virt,
                                                       PhysicalAddress start_phys,
                                                       std::size_t size_bytes, AccessFlags flags,
                                                       CacheMode cache = CacheMode::WriteBack,
                                                       std::uint8_t pkey = 0) noexcept;

  /// @brief Changes write/execute/user permission of every mapped page in the range.
  ///
  /// Unmapped holes are skipped. Copy-on-write pages stay read-only whatever @p flags says.
  /// All or nothing.
  [[nodiscard]] std::expected<void, Error> protect_virtual_range(VirtualAddress start_virt,
                                                                 std::size_t size_bytes,
                                                                 AccessFlags flags) noexcept;

  /// @brief Clears the write bit of every mapped page in the range.
  ///
  /// Unlike protect_virtual_range() this never allocates and cannot fail, which is what fork()
  /// needs once it has committed to sharing pages. Huge pages that straddle the range are
  /// write-protected as a whole.
  void write_protect_range(VirtualAddress start_virt, std::size_t size_bytes) noexcept;

  /// @brief Maps the frames behind [src, src + size) a second time at @p dest_virt.
  [[nodiscard]] std::expected<void, Error> alias_virtual_range(VirtualAddress src_virt,
                                                               VirtualAddress dest_virt,
                                                               std::size_t size_bytes) noexcept;

  /// @brief alias_virtual_range() followed by unmapping the source.
  [[nodiscard]] std::expected<void, Error> move_virtual_range(VirtualAddress src_virt,
                                                              VirtualAddress dest_virt,
                                                              std::size_t size_bytes) noexcept;
};

/// @brief Detects paging features. Runs on every CPU, before initialize_hw().
void early_initialize_hw() noexcept;

/// @brief Programs EFER, CR0, CR4 and the PAT on the calling CPU.
void initialize_hw() noexcept;
}  // namespace kernel::memory::vmm
