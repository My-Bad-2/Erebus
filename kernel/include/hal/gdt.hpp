#pragma once

#include "memory/address.hpp"
#include <array>
#include <cstdint>
#include <utility>

namespace kernel::hw::gdt {
//   syscall: CS = STAR[47:32],     SS = STAR[47:32] + 8
//   sysret:  SS = STAR[63:48] + 8, CS = STAR[63:48] + 16
enum class Selector : uint16_t {
  Null = 0x00,
  KernelCode = 0x08,
  KernelData = 0x10,
  UserData = 0x18, // sysret SS (0x10 + 8) | RPL 3 = 0x1B
  UserCode = 0x20, // sysret CS (0x10 + 16) | RPL 3 = 0x23
  Tss = 0x28       // 16-byte System Segment (spans 0x28 and 0x30)
};

enum class PrivilegeLevel : uint8_t { Ring0 = 0, Ring3 = 3 };
[[nodiscard]] constexpr Selector with_rpl(const Selector sel, const PrivilegeLevel rpl) noexcept {
  return static_cast<Selector>(std::to_underlying(sel) | std::to_underlying(rpl));
}

enum class SegmentFlags : uint64_t {
  Accessed = 1ul << 40,
  ReadWrite = 1ul << 41, // Readable for Code, Writable for Data
  Conforming = 1ul << 42,
  Executable = 1ul << 43, // Code Segment
  UserType = 1ul << 44,   // 1 = Code/Data, 0 = System (TSS/LDT)
  DplRing0 = 0ul << 45,
  DplRing3 = 3ul << 45,
  Present = 1ul << 47,
  LongMode = 1ul << 53,   // 64-bit code segment flag (L)
  Granularity = 1ul << 55 // 4KB page granularity
};

[[nodiscard]] constexpr SegmentFlags operator|(const SegmentFlags lhs, const SegmentFlags rhs) noexcept {
  return static_cast<SegmentFlags>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

[[nodiscard]] constexpr SegmentFlags operator&(const SegmentFlags lhs, const SegmentFlags rhs) noexcept {
  return static_cast<SegmentFlags>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

enum class IstIndex : uint8_t { None = 0, Ist1 = 1, Ist2 = 2, Ist3 = 3, Ist4 = 4, Ist5 = 5, Ist6 = 6, Ist7 = 7 };

struct [[gnu::packed]] TaskStateSegment {
  std::uint32_t reserved0{0};
  std::uint64_t rsp0{0}; // Ring 0 Stack Pointer
  std::uint64_t rsp1{0};
  std::uint64_t rsp2{0};
  std::uint64_t reserved1{0};
  std::array<std::uint64_t, 7> ist{}; // Interrupt Stack Tables
  std::uint64_t reserved2{0};
  std::uint16_t reserved3{0};
  std::uint16_t iopb_offset{sizeof(TaskStateSegment)}; // Disable I/O bitmap
};

struct [[gnu::packed]] GdtDescriptor {
  std::uint16_t size;
  std::uint64_t offset;
};

class GdtEntry {
  friend class GlobalDescriptorTable;
  explicit constexpr GdtEntry(const uint64_t value) : m_value(value) {}
  uint64_t m_value{0};

public:
  consteval GdtEntry() = default;

  [[nodiscard]] static consteval GdtEntry create_segment(const bool is_code, const PrivilegeLevel dpl) {
    auto flags = SegmentFlags::UserType | SegmentFlags::Present | SegmentFlags::ReadWrite;

    if (is_code) {
      flags = flags | SegmentFlags::Executable | SegmentFlags::LongMode;
    }

    if (dpl == PrivilegeLevel::Ring3) {
      flags = flags | SegmentFlags::DplRing3;
    }

    return GdtEntry(std::to_underlying(flags));
  }

  [[nodiscard]] constexpr uint64_t raw() const { return m_value; }
};

class alignas(std::hardware_destructive_interference_size) GlobalDescriptorTable {
public:
  static constexpr std::size_t TotalEntries = 7; // Null, KC, KD, UD, UC, TSS(2)
private:
  alignas(16) std::array<GdtEntry, TotalEntries> m_entries{};
  TaskStateSegment m_tss{};

  [[nodiscard]] static std::array<GdtEntry, 2> make_tss_descriptors(const TaskStateSegment &tss) noexcept;

public:
  constexpr GlobalDescriptorTable() noexcept {
    m_entries[0] = GdtEntry();                                             // Null
    m_entries[1] = GdtEntry::create_segment(true, PrivilegeLevel::Ring0);  // 0x08: Kernel Code
    m_entries[2] = GdtEntry::create_segment(false, PrivilegeLevel::Ring0); // 0x10: Kernel Data
    m_entries[3] = GdtEntry::create_segment(false, PrivilegeLevel::Ring3); // 0x18: User Data
    m_entries[4] = GdtEntry::create_segment(true, PrivilegeLevel::Ring3);  // 0x20: User Code
  }

  void load() noexcept;

  GlobalDescriptorTable &set_kernel_stack(const memory::VirtualAddress stack_top) noexcept {
    m_tss.rsp0 = stack_top.value();
    return *this;
  }

  GlobalDescriptorTable &set_interrupt_stack(const IstIndex idx, const memory::VirtualAddress stack_top) noexcept {
    if (idx != IstIndex::None) {
      m_tss.ist[std::to_underlying(idx) - 1] = stack_top.value();
    }

    return *this;
  }
};
} // namespace kernel::hw::gdt