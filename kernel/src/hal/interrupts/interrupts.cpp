#include "hal/interrupts.hpp"

#include "hal/gdt.hpp"
#include "hal/hal.hpp"
#include "memory/memory.hpp"
#include "utility"
#include "utils/logger.hpp"

namespace kernel::hw::interrupts {
namespace {
namespace fred_config {
enum class FredMsr : std::uint32_t {
  Rsp0 = 0x1CC,    // Ring 0 Stack Pointer
  Rsp1 = 0x1CD,    // Ring 1 Stack Pointer
  Rsp2 = 0x1CE,    // Ring 2 Stack Pointer
  Rsp3 = 0x1CF,    // Ring 3 Stack Pointer
  Stklvls = 0x1D0, // Exception Stack Levels
  Config = 0x1D4   // Master Configuration
};

enum class FredStackLevel : std::uint8_t {
  Level0 = 0, // Default Kernel Stack
  Level1 = 1, // Critical Rescue Stack (#DF, #MC, NMI)
  Level2 = 2, // Unused/Reserved
  Level3 = 3  // Unused/Reserved
};

enum class FredRedzone : std::uint8_t {
  Disabled = 0,     // Used by default
  SystemV_128B = 2, // 128 bytes = Two 64-byte cache lines
};

struct FredConfig {
private:
  std::uint64_t m_data{0};

public:
  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(FredStackLevel, software_event_stack_level, 0, 2)
  BF_RW(FredStackLevel, critical_event_stack_level, 2, 2)
  BF_RW(FredRedzone, redzone_reservation, 6, 3)
  BF_RW(FredStackLevel, maskable_int_stack_level, 9, 2)
  BF_RW(std::uint64_t, entrypoint_base, 12, 52)
};

struct FredStklvls {
private:
  std::uint64_t m_data{0};

public:
  std::uint64_t raw() const noexcept { return m_data; }

  [[nodiscard]] constexpr uint8_t get_exception_level(const InterruptVector vector) const noexcept {
    if (vector >= InterruptVector::ExternalInterruptStart) [[unlikely]] {
      return 0;
    }

    return (m_data >> (std::to_underlying(vector) * 2)) & 0b11;
  }

  constexpr auto with_exception_level(const InterruptVector vector, const FredStackLevel level) const noexcept {
    FredStklvls copy = *this;
    if (vector < InterruptVector::ExternalInterruptStart) [[likely]] {
      const std::uint64_t shift = std::to_underlying(vector) * 2;
      copy.m_data = (copy.m_data & ~(3ul << shift)) | ((static_cast<uint64_t>(level) & 0x3) << shift);
    }

    return copy;
  }
};

struct FredPointer {
private:
  std::uint64_t m_data{0};

public:
  std::uint64_t raw() const noexcept { return m_data; }
  BF_RW(std::uint64_t, ptr, 0, 63);
};

void initialize(const memory::VirtualAddress ring0_rsp, memory::VirtualAddress ring0_critical_rsp) noexcept {
  FredConfig config{};

  const auto entrypoint_addr = memory::VirtualAddress{&fred_entrypoint};
  if (!entrypoint_addr.is_aligned(memory::PAGE_SIZE)) [[unlikely]] {
    utils::logger::fatal("FRED entrypoint address is not aligned to page size!\n");
  }

  config.set_entrypoint_base(entrypoint_addr.value() >> memory::PAGE_SHIFT_4KB);
  config.set_software_event_stack_level(FredStackLevel::Level0);
  config.set_maskable_int_stack_level(FredStackLevel::Level0);
  config.set_critical_event_stack_level(FredStackLevel::Level1);
  config.set_redzone_reservation(FredRedzone::Disabled);

  write::msr(std::to_underlying(FredMsr::Config), config.raw());

  FredStklvls stklvls{};
  stklvls = stklvls.with_exception_level(InterruptVector::NonMaskableInterrupt, FredStackLevel::Level1);
  stklvls = stklvls.with_exception_level(InterruptVector::DoubleFault, FredStackLevel::Level1);
  stklvls = stklvls.with_exception_level(InterruptVector::MachineCheck, FredStackLevel::Level1);
  write::msr(std::to_underlying(FredMsr::Stklvls), stklvls.raw());

  FredPointer rsp0{};
  rsp0.set_ptr(ring0_rsp.value());
  write::msr(std::to_underlying(FredMsr::Rsp0), rsp0.raw());

  FredPointer rsp1{};
  rsp1.set_ptr(ring0_critical_rsp.value());
  write::msr(std::to_underlying(FredMsr::Rsp1), rsp1.raw());

  write::msr(std::to_underlying(FredMsr::Rsp2), 0);
  write::msr(std::to_underlying(FredMsr::Rsp3), 0);

  CR4 cr4 = read::cr4();
  cr4.set_fred(true);
  write::cr4(cr4);
}
} // namespace fred_config

namespace legacy {
enum class GateType : std::uint8_t {
  Interrupt = 0xE, // Clears RFLAGS.IF
  Trap = 0xF,      // Leaves RFLAGS.IF unchanged
};

struct [[gnu::packed]] IDTEntry {
  std::uint16_t offset_low;
  std::uint16_t selector;
  std::uint8_t ist;
  std::uint8_t type_attr;
  std::uint16_t offset_mid;
  std::uint32_t offset_high;
  std::uint32_t reserved;

  constexpr void set_handler(void *handler, gdt::Selector sel, const GateType type, const EventCpl dpl,
                             const std::uint8_t ist_idx = 0) {
    const auto addr = reinterpret_cast<std::uint64_t>(handler);

    offset_low = static_cast<std::uint16_t>(addr & 0xFFFF);
    offset_mid = static_cast<std::uint16_t>((addr >> 16) & 0xFFFF);
    offset_high = static_cast<std::uint32_t>((addr >> 32) & 0xFFFFFFFF);

    selector = static_cast<std::uint16_t>(sel);
    ist = ist_idx & 0x07;
    reserved = 0;

    constexpr std::uint8_t PRESENT_BIT = 1 << 7;
    type_attr = PRESENT_BIT | (std::to_underlying(dpl) << 5) | std::to_underlying(type);
  }
};

struct [[gnu::packed]] IDTR {
  std::uint16_t limit;
  std::uint64_t base;
};

alignas(16) std::array<IDTEntry, IDT_SIZE> g_idt_table{};

void initialize() noexcept {
  static auto once = [] {
    for (std::size_t vec = 0; vec < IDT_SIZE; ++vec) {
      const auto handler = reinterpret_cast<void *>(legacy_idt_stubs[vec]);
      constexpr auto dpl = EventCpl::Ring0;

      std::uint8_t ist_idx = 0;
      if (vec == std::to_underlying(InterruptVector::NonMaskableInterrupt) ||
          vec == std::to_underlying(InterruptVector::DoubleFault) ||
          vec == std::to_underlying(InterruptVector::MachineCheck)) {
        ist_idx = 1;
      }

      g_idt_table[vec].set_handler(handler, gdt::Selector::KernelCode, GateType::Interrupt, dpl, ist_idx);
    }

    return true;
  }();

  IDTR idtr{
      .limit = static_cast<std::uint16_t>(sizeof(g_idt_table) - 1),
      .base = reinterpret_cast<std::uint64_t>(g_idt_table.data()),
  };

  asm volatile("lidt %0" : /* No Output */ : "m"(idtr) : "memory");
}
} // namespace legacy
} // namespace

void initialize(memory::VirtualAddress ring0_stack, memory::VirtualAddress critical_stack) noexcept {
  if (profile_manager.get_current()->has<Feature::FRED>()) {
    fred_config::initialize(ring0_stack, critical_stack);
  } else {
    legacy::initialize();
  }
}
} // namespace kernel::hw::interrupts