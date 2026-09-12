#pragma once

#include <bitfield.hpp>
#include <cstdint>
#include <utility>

namespace kernel::memory::vmm {
enum class Error {
  Success = 0,
  OutOfMemory,
  AlreadyMapped,
  NotMapped,
  InvalidAlignment,
  InvalidFlags,
  CasFailed,
  SecurityViolation,
  OutOfBounds,
  NotImplemented,
  StackOverflow,
  InvalidState,
};

enum class PageSize : std::uint8_t {
  Size4K = 1,
  Size2M = 2,
  Size1G = 3,
};

enum class AccessFlags : std::uint32_t {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Execute = 1 << 2,
  User = 1 << 3,
  Global = 1 << 4,
  Shared = 1 << 5,
  CopyOnWrite = 1 << 6,
  Swapped = 1 << 7, // Page is evicted
  Accessed = 1 << 8,
  Dirty = 1 << 9,
  Mmio = 1 << 10, // Memory Mapped IO (Forces Uncacheable)
  Stack = 1 << 11,
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

constexpr AccessFlags &operator|=(AccessFlags &a, const AccessFlags b) noexcept { return a = a | b; }

constexpr AccessFlags &operator&=(AccessFlags &a, const AccessFlags b) noexcept { return a = a & b; }

constexpr bool has_flag(const AccessFlags flags, const AccessFlags flag) noexcept { return (flags & flag) == flag; }

enum class CacheMode : std::uint8_t {
  WriteBack,        // Normal RAM
  WriteThrough,     // Writes update cache and RAM synchronously
  UncacheableMinus, // UC-, can be overridden by MTRRs to WriteCombining
  Uncacheable,      // Strict MMIO
  WriteCombining,   // Framebuffers
  WriteProtected    // reads hit cache, writes go to RAM
};

enum class ArchFlags : std::uint64_t {
  Present = 1ul << 0,
  ReadWrite = 1ul << 1,
  User = 1ul << 2,
  PWT = 1ul << 3,
  PCD = 1ul << 4,
  Accessed = 1ul << 5,
  Dirty = 1ul << 6,
  PatLeaf = 1ul << 7,
  Huge = 1ul << 7,
  Global = 1ul << 8,
  PatHuge = 1ul << 12,
  NX = 1ul << 63
};

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

  // If mapping a 4KB page (Leaf), bit 7 is the Page Attribute Table (PAT) bit.
  // If mapping a 2MB/1GB page, bit 7 is the Page Size (Huge) bit.
  BF_BIT_RW(pat_4k, 7)
  BF_BIT_RW(huge, 7)
  BF_BIT_RW(global, 8)
  BF_BIT_RW(cow, 9)      // OS specific: Triggers Copy-on-Write
  BF_BIT_RW(shared, 10)  // OS specific: Mapped in multiple address spaces
  BF_BIT_RW(swapped, 11) // OS specific: Evicted to disk

  // For 2MB/1GB pages, PAT is moved to bit 12.
  BF_BIT_RW(pat_large, 12)

  // Page Frame Numbers
  BF_RW(std::uint64_t, pfn_4k, 12, 40) // 4KB PFN
  BF_RW(std::uint64_t, pfn_2m, 21, 31) // 2MB PFN
  BF_RW(std::uint64_t, pfn_1g, 30, 22) // 1GB PFN

  BF_RW(std::uint8_t, avl_high, 52, 6)
  BF_BIT_RW(stack, 58)             // OS specific: Is stack memory
  BF_RW(std::uint8_t, pkey, 59, 4) // Protection Keys (PKRU)
  BF_BIT_RW(nx, 63)                // No-Execute
};
} // namespace kernel::memory::vmm