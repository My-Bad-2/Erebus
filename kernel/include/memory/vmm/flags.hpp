#pragma once

#include <bitfield.hpp>
#include <cstdint>

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
  Mmio = 1 << 10 // Memory Mapped IO (Forces Uncacheable)
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

using PteLeafSchema = klib::BitfieldSchema<klib::Bit<"present", 0>,        // present
                                            klib::Bit<"rw", 1>,             // write
                                            klib::Bit<"user", 2>,           // user-supervisor
                                            klib::Bit<"pwt", 3>,            // write through
                                            klib::Bit<"pcd", 4>,            // cache disable
                                            klib::Bit<"accessed", 5>,       // accessed
                                            klib::Bit<"dirty", 6>,          // is dirty
                                            klib::Bit<"pat", 7>,            // pat
                                            klib::Bit<"global", 8>,         // global pages
                                            klib::Bit<"cow", 9>,            // Triggers Copy-on-Write in the #PF handler
                                            klib::Bit<"shared", 10>,        // Mapped in multiple address spaces
                                            klib::Bit<"swapped", 11>,       // Evicted to disk. PFN is a swap index.
                                            klib::Field<"pfn", 12, 40>,     // pfn
                                            klib::Field<"avl_high", 52, 7>, // available to used
                                            klib::Field<"pkey", 59, 4>,     // pkru
                                            klib::Bit<"nx", 63>>;

using PteHugeSchema = klib::BitfieldSchema<klib::Bit<"present", 0>,        // present
                                           klib::Bit<"rw", 1>,             // write
                                           klib::Bit<"user", 2>,           // user-supervisor
                                           klib::Bit<"pwt", 3>,            // write through
                                           klib::Bit<"pcd", 4>,            // cache disable
                                           klib::Bit<"accessed", 5>,       // accessed
                                           klib::Bit<"dirty", 6>,          // is dirty
                                           klib::Bit<"huge", 7>,           // pat
                                           klib::Bit<"global", 8>,         // global pages
                                           klib::Bit<"cow", 9>,            // Triggers Copy-on-Write in the #PF handler
                                           klib::Bit<"shared", 10>,        // Mapped in multiple address spaces
                                           klib::Bit<"swapped", 11>,       // Evicted to disk. PFN is a swap index.
                                           klib::Bit<"pat", 12>,           // pat bit
                                           klib::Field<"pfn", 21, 31>,     // pfn
                                           klib::Field<"avl_high", 52, 7>, // available to used
                                           klib::Field<"pkey", 59, 4>,     // pkru
                                           klib::Bit<"nx", 63>>;

using Pte1gSchema = klib::BitfieldSchema<klib::Bit<"present", 0>,        // present
                                         klib::Bit<"rw", 1>,             // write
                                         klib::Bit<"user", 2>,           // user-supervisor
                                         klib::Bit<"pwt", 3>,            // write through
                                         klib::Bit<"pcd", 4>,            // cache disable
                                         klib::Bit<"accessed", 5>,       // accessed
                                         klib::Bit<"dirty", 6>,          // is dirty
                                         klib::Bit<"huge", 7>,           // pat
                                         klib::Bit<"global", 8>,         // global pages
                                         klib::Bit<"cow", 9>,            // Triggers Copy-on-Write in the #PF handler
                                         klib::Bit<"shared", 10>,        // Mapped in multiple address spaces
                                         klib::Bit<"swapped", 11>,       // Evicted to disk. PFN is a swap index.
                                         klib::Bit<"pat", 12>,           // pat bit
                                         klib::Field<"pfn", 30, 22>,     // pfn
                                         klib::Field<"avl_high", 52, 7>, // available to used
                                         klib::Field<"pkey", 59, 4>,     // pkru
                                         klib::Bit<"nx", 63>>;

struct PteSchema {
  std::uint64_t raw{0};

  constexpr PteSchema() noexcept = default;
  constexpr explicit PteSchema(const std::uint64_t val) noexcept : raw{val} {}

  constexpr operator std::uint64_t() const noexcept { return raw; }

  [[nodiscard]] constexpr PteLeafSchema small() const noexcept { return PteLeafSchema{raw}; }
  [[nodiscard]] constexpr PteHugeSchema huge() const noexcept { return PteHugeSchema{raw}; }
  [[nodiscard]] constexpr Pte1gSchema gig() const noexcept { return Pte1gSchema{raw}; }
};
} // namespace kernel::memory::vmm