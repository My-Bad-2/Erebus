/// @file flags.hpp
/// @brief Architecture-neutral mapping attributes and the raw x86-64 page table entry layout.

#pragma once

#include <bitfield.hpp>
#include <cstdint>
#include <utility>

#include "core/errors.hpp"

namespace kernel::memory::vmm {
/// @brief Mapping granularity. The value is the paging level that holds the leaf entry.
enum class PageSize : std::uint8_t {
  Size4K = 1,
  Size2M = 2,
  Size1G = 3,
};

/// @brief What a mapping allows, plus a few software-only attributes.
///
/// Read/Write/Execute/User/Global map directly onto PTE bits. CopyOnWrite, Shared, Swapped and
/// Stack live in the PTE's software-available bits. Mmio and Populate never reach the page
/// tables: they only steer the VMA layer.
enum class AccessFlags : std::uint32_t {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Execute = 1 << 2,
  User = 1 << 3,
  Global = 1 << 4,
  Shared = 1 << 5,
  CopyOnWrite = 1 << 6,
  Swapped = 1 << 7,  // Page is evicted
  Accessed = 1 << 8,
  Dirty = 1 << 9,
  Mmio = 1 << 10,  // Device memory; callers pick an uncached CacheMode
  Stack = 1 << 11,
  Populate = 1 << 12,  // Fault the whole range in at allocation time
};

constexpr AccessFlags operator|(const AccessFlags a, const AccessFlags b) noexcept {
  return static_cast<AccessFlags>(std::to_underlying(a) | std::to_underlying(b));
}

constexpr AccessFlags operator&(const AccessFlags a, const AccessFlags b) noexcept {
  return static_cast<AccessFlags>(std::to_underlying(a) & std::to_underlying(b));
}

constexpr AccessFlags operator~(const AccessFlags a) noexcept {
  return static_cast<AccessFlags>(~std::to_underlying(a));
}

constexpr AccessFlags& operator|=(AccessFlags& a, const AccessFlags b) noexcept {
  return a = a | b;
}

constexpr AccessFlags& operator&=(AccessFlags& a, const AccessFlags b) noexcept {
  return a = a & b;
}

/// @brief True if *all* bits of @p flag are set in @p flags.
constexpr bool has_flag(const AccessFlags flags, const AccessFlags flag) noexcept {
  return (flags & flag) == flag;
}

/// @brief The flags a user of `protect()` is allowed to change. The rest describe what a mapping
///        is (shared, copy-on-write, a stack, ...) and are kept as they are.
inline constexpr AccessFlags PROTECTION_FLAGS =
    AccessFlags::Read | AccessFlags::Write | AccessFlags::Execute;

/// @brief Memory type of a mapping, encoded through the PAT (see initialize_hw()).
enum class CacheMode : std::uint8_t {
  WriteBack,         // Normal RAM
  WriteThrough,      // Writes update cache and RAM synchronously
  UncacheableMinus,  // UC-, can be overridden by MTRRs to WriteCombining
  Uncacheable,       // Strict MMIO
  WriteCombining,    // Framebuffers
  WriteProtected     // reads hit cache, writes go to RAM
};

/// @brief Bit layout of an x86-64 paging structure entry.
///
/// The same layout serves every level; which fields are meaningful depends on whether the entry
/// is a leaf. Bit 7 is the PAT bit in a 4 KiB leaf but the page-size bit in a PDE/PDPTE, and a
/// huge leaf moves its PAT bit to bit 12 (which is why its frame number starts higher).
class PTEntry {
  std::uint64_t m_data;

 public:
  constexpr explicit PTEntry(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  operator std::uint64_t() const noexcept { return m_data; }

  BF_BIT_RW(present, 0)
  BF_BIT_RW(rw, 1)
  BF_BIT_RW(user, 2)
  BF_BIT_RW(pwt, 3)
  BF_BIT_RW(pcd, 4)
  BF_BIT_RW(accessed, 5)
  BF_BIT_RW(dirty, 6)

  BF_BIT_RW(pat_4k, 7)  // 4 KiB leaf only
  BF_BIT_RW(huge, 7)    // PDE/PDPTE only
  BF_BIT_RW(global, 8)
  BF_BIT_RW(cow, 9)       // Software: write must copy the frame first
  BF_BIT_RW(shared, 10)   // Software: mapped in multiple address spaces
  BF_BIT_RW(swapped, 11)  // Software: evicted, the frame field holds a swap slot

  BF_BIT_RW(pat_large, 12)  // 2 MiB / 1 GiB leaf only

  // Frame numbers; the field used depends on the size of the leaf (or 4 KiB for a directory).
  BF_RW(std::uint64_t, pfn_4k, 12, 40)
  BF_RW(std::uint64_t, pfn_2m, 21, 31)
  BF_RW(std::uint64_t, pfn_1g, 30, 22)

  BF_RW(std::uint8_t, avl_high, 52, 6)
  BF_BIT_RW(stack, 58)              // Software: stack memory
  BF_RW(std::uint8_t, pkey, 59, 4)  // Protection key (PKRU)
  BF_BIT_RW(nx, 63)                 // No-execute
};
}  // namespace kernel::memory::vmm
