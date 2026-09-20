#pragma once

#include <array>
#include <bitfield.hpp>
#include <cstdint>

#include "memory/address.hpp"

namespace kernel::hw::interrupts {
namespace fred {
enum class EventType : std::uint8_t {
  ExternalInterrupt = 0,     // Maskable hardware interrupts
  Nmi = 2,                   // Non-Maskable Interrupts
  HardwareException = 3,     // #PF, #GP, #DB, etc.
  SoftwareInterrupt = 4,     // INT n
  PrivSoftwareException = 5, // INT1 (ICEBP)
  SoftwareException = 6,     // INT3, INTO
  OtherEvent = 7             // SYSCALL, SYSENTER
};

struct CsInfo {
  uint64_t m_data;
  BF_RW(uint16_t, cs_selector, 0, 16)
  BF_RW(uint8_t, sl, 16, 2) // Stack level
  BF_BIT_RW(wfe, 18)        // Wait for ENDBR
};

struct EventInfo {
private:
  std::uint64_t m_data{0};

public:
  BF_RO(std::uint16_t, ss, 0, 16);
  BF_BIT_RO(stb, 16); // STI Blocking
  BF_BIT_RO(sys, 17); // software interrupt triggered by syscall, sysenter, or INT n
  BF_BIT_RO(nmi, 18); // NMI delivered

  // For exernal interrupt, the vector is provided by the LAPIC.
  // For a NMI, the vector is 2.
  // For a hw interrupt, vector is determined by the exception
  // INT n provides `n` as the vector.
  // INT1 -> 1 ; INT3 -> 3; INTO -> 4
  // SYSCALL -> 1 ; SYSENTER -> 2
  BF_RO(std::uint8_t, vector, 32, 8);
  BF_RO(EventType, type, 48, 4);
  BF_BIT_RO(enclave, 56);                // enclave / SGX context
  BF_BIT_RO(long_mode, 57);              // 1 -> Event happened in x64 mode
  BF_BIT_RO(nst, 58);                    // Nested context
  BF_RO(std::uint8_t, instr_len, 60, 4); // only for sw (sys) interrupts
};
} // namespace fred

struct RegisterContext {
  std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  std::uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
};

struct LegacyFrame {
  RegisterContext gprs;

  std::uint64_t vector, error_code;
  std::uint64_t rip, cs, rflags, rsp, ss;
};

struct FredFrame {
  RegisterContext gprs;

  std::uint64_t padding;

  std::uint64_t error_code, rip;
  fred::CsInfo cs_info;
  std::uint64_t rflags, rsp;
  fred::EventInfo event_info;
  std::uint64_t event_data, reserved;
};

void dispatch_legacy(LegacyFrame *frame) noexcept;
void dispatch_legacy_nmi(LegacyFrame *frame) noexcept;
void dispatch_fred(FredFrame *frame) noexcept;

enum class EventClass : std::uint8_t {
  Interrupt, // Timer, Keyboard, etc.
  Exception, // #PF, #GP, etc.
  NMI,       // Non-Maskable Interrupts
  Syscall,   // Syscall/Sysenter or INT n
  Unknown,
};

enum class EventCpl : std::uint8_t {
  Ring0 = 0,
  Ring3 = 3,
};

struct Event {
  EventClass category;
  std::uint8_t vector;
  EventCpl cpl;
  std::uint16_t error_code;

  std::uint64_t payload;
  std::uint64_t rip, rsp, rflags;
  RegisterContext *gprs;
};

enum class InterruptVector : std::uint8_t {
  DivideError = 0,                   // #DE
  Debug = 1,                         // #DB
  NonMaskableInterrupt = 2,          // NMI
  Breakpoint = 3,                    // #BP
  Overflow = 4,                      // #OF
  BoundRangeExceeded = 5,            // #BR
  InvalidOpcode = 6,                 // #UD
  DeviceNotAvailable = 7,            // #NM (FPU/Math Coprocessor missing)
  DoubleFault = 8,                   // #DF (Has Error Code)
  CoprocessorSegmentOverrun = 9,     // Legacy (Unsupported on modern x86)
  InvalidTSS = 10,                   // #TS (Has Error Code)
  SegmentNotPresent = 11,            // #NP (Has Error Code)
  StackSegmentFault = 12,            // #SS (Has Error Code)
  GeneralProtectionFault = 13,       // #GP (Has Error Code)
  PageFault = 14,                    // #PF (Has Error Code)
  x87FloatingPointException = 16,    // #MF (Math Fault)
  AlignmentCheck = 17,               // #AC (Has Error Code)
  MachineCheck = 18,                 // #MC (Fatal Hardware Error)
  SIMDFloatingPointException = 19,   // #XM/#XF
  VirtualizationException = 20,      // #VE
  ControlProtectionException = 21,   // #CP (Has Error Code, CET related)
  HypervisorInjectionException = 28, // #HV
  VMMCommunicationException = 29,    // #VC (Has Error Code)
  SecurityException = 30,            // #SX (Has Error Code)

  ExternalInterruptStart = 32,

  LocalApicSpurious = 255,
  LocalApicTimer = 254,
  IpiSchedule = 253,
  IpiTlbShootdown = 252,
};

[[nodiscard]] consteval bool has_error_code(InterruptVector vector) noexcept {
  const auto v = static_cast<std::uint8_t>(vector);
  return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30;
}

[[nodiscard]] consteval bool is_exception(InterruptVector vector) noexcept {
  return static_cast<std::uint8_t>(vector) < static_cast<std::uint8_t>(InterruptVector::ExternalInterruptStart);
}

using StubPtr = void (*)();
constexpr std::size_t IDT_SIZE = 256;
extern std::array<StubPtr, IDT_SIZE> legacy_idt_stubs;

[[gnu::naked]] void fred_entrypoint() noexcept;
void initialize(memory::VirtualAddress ring0_stack, memory::VirtualAddress critical_stack) noexcept;
} // namespace kernel::hw::interrupts