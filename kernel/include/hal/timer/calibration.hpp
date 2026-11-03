#pragma once

#include <cstdint>

#include "hal/smp/cpu_info.hpp"

namespace kernel::hw::timer {
enum class Source : std::uint8_t {
  None,
  HypervisorLeaf,        // VMware / KVM / QEMU paravirtualized timing leaf
  IntelCpuid15,          // Intel SDM Vol. 2A Leaf 15H (Crystal Clock ratio)
  IntelCpuid16,          // Intel SDM Vol. 2A Leaf 16H (Processor Base MHz)
  IntelMsrPlatformInfo,  // Intel SDM Vol. 4 MSR_PLATFORM_INFO (0xCE)
  AmdMsrPstateDef,       // AMD BKDG / PPR MSRC001_00[6B:64] P-State 0
  HpetSoftware,          // Software-timed TSC against HPET Main Counter
  HpetDirect,            // Direct HPET Main Counter timekeeping
  PmTimerSoftware,       // Software-timed TSC against the ACPI PM timer
  PmTimerDirect,         // Direct ACPI PM timer timekeeping
};

struct Result {
  std::uint64_t tsc_hz{0};
  std::uint64_t apic_bus_hz{0};
  Source source{Source::None};
};

bool is_tsc_invariant(const CpuInfo& cpu) noexcept;
Result probe_hardware_frequencies(const CpuInfo& cpu) noexcept;
}  // namespace kernel::hw::timer