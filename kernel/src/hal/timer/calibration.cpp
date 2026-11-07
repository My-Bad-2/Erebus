// For a better explanation, visit: https://osdev.wiki/wiki/TSC

#include "hal/timer/calibration.hpp"

#include "drivers/clock.hpp"
#include "hal/io.hpp"
#include "hal/msr.hpp"

namespace kernel::hw::timer {
namespace {
Result probe_hypervisor(const CpuInfo& cpu) noexcept {
  if (!cpu.is_virtualized()) {
    return {};
  }

  auto [max_hv, _, _, _] = cpu.query(0x40000000);
  if (max_hv < 0x40000010) {
    return {};
  }

  auto [tsc_khz, apic_khz, _, _] = cpu.query(0x40000010);

  Result res{};
  if (tsc_khz >= 100'000 && tsc_khz <= 20'000'000) {
    res.source = Source::HypervisorLeaf;
    res.tsc_hz = static_cast<std::uint64_t>(tsc_khz) * 1'000;
  }

  if (apic_khz >= 1'000 && apic_khz <= 10'000'000) {
    res.apic_bus_hz = static_cast<std::uint64_t>(apic_khz) * 1'000;
  }

  return res;
}

std::uint64_t lookup_intel_crystal_hz(const CpuInfo& cpu) noexcept {
  switch (cpu.microarchitecture()) {
    case UArch::Skylake... UArch::KabyLake_CoffeeLake:
      return 24'000'000ul;  // 24 MHz (6th-9th Gen Core Client)
    case UArch::CascadeLake:
      return 25'000'000ul;  // 25 MHz (SkyLake-X / Cascade Lake Xeon)
    case UArch::Goldmont:
      // Gemini Lake (0x7A) = 25 MHz, Apollo Lake (0x5C) = 19.2 MHz
      return (cpu.model() == 0x7A) ? 25'000'000ul : 19'200'000ul;
    default:
      return 0;
  }
}

Result probe_intel(const CpuInfo& cpu) noexcept {
  if (cpu.vendor() != Vendor::Intel) return {};

  Result res{};

  // Intel CPUID Leaf 0x15 (Skylake 2015+)
  // Source: Intel SDM Vol. 1 Table 21-53
  //         EAX = Denominator, EBX = Numerator, ECX = Core Crystal Clock (Hz).
  //         TSC_Hz = (ECX * EBX) / EAX, and LAPIC Timer Ticks @ ECX.
  if (cpu.max_leaf() >= 0x15) {
    auto [denom, numer, crystal_hz_, _] = cpu.query(0x15);

    std::uint64_t crystal_hz = crystal_hz_;
    // Workaround for Skylake/KabyLake/Goldmont where CPUID.15H:ECX is 0
    if (crystal_hz == 0) crystal_hz = lookup_intel_crystal_hz(cpu);

    if (crystal_hz != 0 && denom != 0 && numer != 0) {
      res.tsc_hz = (crystal_hz * numer) / denom;
      res.apic_bus_hz = crystal_hz;
      res.source = Source::IntelCpuid15;
      return res;
    }
  }

  // Intel CPUID Leaf 0x16 (Processor Frequency Information Leaf)
  // Source: Intel SDM Vol. 1 Table 21-54: EAX[15:0] = Base MHz, ECX[15:0] = Bus Reference MHz.
  if (cpu.base_mhz() >= 100 && cpu.base_mhz() <= 20'000) {
    res.tsc_hz = static_cast<std::uint64_t>(cpu.base_mhz()) * 1'000'000ul;
    res.source = Source::IntelCpuid16;
    return res;
  }

  // Pre-Skylake intel via MSR_PLATFORM_INFO (0xCE).
  if (!cpu.is_virtualized()) {
    const MsrPlatformInfo info{read::msr(MSR_PLATFORM_INFO)};
    const std::uint64_t ratio = info.get_max_non_turbo_ratio();

    if (ratio != 0) {
      switch (cpu.microarchitecture()) {
        case UArch::Nehalem... UArch::Westmere: {
          // SOURCE: Intel SDM Vol 3B. 22.7.3.2 (BCLK = 133.333 MHz [400 MHz / 3]).
          res.tsc_hz = ratio * 400'000'000ul / 3ul;
          res.apic_bus_hz = 400'000'000ul / 3ul;
          res.source = Source::IntelMsrPlatformInfo;
          return res;
        }
        case UArch::SandyBridge... UArch::Broadwell: {
          // SOURCE: Intel SDM Vol 3B. 22.7.3.1 (BCLK = 100 MHz).
          res.tsc_hz = ratio * 100'000'000ul;
          res.apic_bus_hz = 100'000'000ul;
          res.source = Source::IntelMsrPlatformInfo;
          return res;
        }
        default:
          break;
      }
    }
  }

  return res;
}

std::uint32_t read_amd_num_boost_states(const CpuInfo& cpu) noexcept {
  if (!cpu.has<Feature::AMD_CPB>()) return 0;

  const AmdNbCfgMsr nb_cfg{read::msr(MSR_AMD_NB_CFG)};
  if (!nb_cfg.get_enable_cf8_ext_cfg()) {
    write::msr(MSR_AMD_NB_CFG, nb_cfg.with_enable_cf8_ext_cfg().raw());
  }

  // PCI Address for Bus 0, Device 0x18 (Node 0), Function 4, Extended Offset 0x15C:
  // Bit 31 = Enable, Bits [27:24] = ExtReg[11:8] (0x1), Bits [15:11] = Dev (0x18),
  // Bits [10:8] = Func (4), Bits [7:2] = Reg[7:2] (0x5C)
  constexpr std::uint32_t PCI_CF8 = (1 << 31) | (1 << 24) | (0x18 << 11) | (4 << 8) | 0x5C;

  constexpr IoResource pci_cfg{IoType::PMIO, 0xCF8};
  pci_cfg.write<std::uint32_t>(0, PCI_CF8);

  // Read 0xCFC
  const AmdCpbControlReg cpb_ctrl{pci_cfg.read<std::uint32_t>(4)};

  if (!nb_cfg.get_enable_cf8_ext_cfg()) {
    write::msr(MSR_AMD_NB_CFG, nb_cfg.raw());
  }

  if (cpb_ctrl.raw() == std::numeric_limits<std::uint32_t>::max()) {
    return 0;
  }

  const std::uint32_t raw_boost = cpb_ctrl.get_num_boost_states();
  return cpu.microarchitecture() == UArch::K10 ? raw_boost & 0x1 : raw_boost & 0x7;
}

Result probe_amd(const CpuInfo& cpu) noexcept {
  // The P-state MSRs describe the real silicon; a hypervisor's TSC need not run at P0.
  if (cpu.vendor() != Vendor::Amd || cpu.is_virtualized()) {
    return {};
  }

  Result res{};

  switch (cpu.microarchitecture()) {
    case UArch::Zen1... UArch::Zen4: {
      // P0 COF (Hz) = (CpuFid[7:0] * 200 MHz) / CpuDfsId[13:8]
      const MsrZenPStateP0 p0{read::msr(MSR_AMD_PSTATE_DEF_BASE)};
      const std::uint64_t fid = p0.get_cpu_fid();
      const std::uint64_t dfs_id = p0.get_cpu_dfs_id();

      if (p0.get_pstate_en() && fid >= 0x10 && dfs_id != 0) {
        res.tsc_hz = fid * 200'000'000ul / dfs_id;
        res.source = Source::AmdMsrPstateDef;
      }

      return res;
    }
    case UArch::Zen5: {
      // P0 COF (Hz) = CpuFid[11:0] * 5 MHz (where 5 MHz = 100 MHz REFCLK / 20)
      const MsrZen5PStateP0 p0{read::msr(MSR_AMD_PSTATE_DEF_BASE)};
      const std::uint64_t fid = p0.get_cpu_fid();

      if (p0.get_pstate_en() && fid >= 0x10) {
        res.tsc_hz = fid * 5'000'000ul;
        res.source = Source::AmdMsrPstateDef;
      }

      return res;
    }
    case UArch::K10... UArch::Jaguar_Puma: {
      // Software P0 = MSRC0010064 + D18F4x15C[NumBoostStates]
      // CoreCOF = (100 MHz * (CpuFid[5:0] + 10h)) >> CpuDid[8:6]
      const std::uint32_t boost_states = read_amd_num_boost_states(cpu);
      if (boost_states < 8) {
        const MsrLegacyPStateP0 p0{read::msr(MSR_AMD_PSTATE_DEF_BASE + boost_states)};
        const std::uint64_t fid = p0.get_cpu_fid();
        const std::uint64_t did = p0.get_cpu_did();

        if (p0.get_pstate_en() && did <= 4) {
          res.tsc_hz = (100'000'000ul * (fid + 0x10)) >> did;
          res.source = Source::AmdMsrPstateDef;
        }
      }

      return res;
    }
    default:
      return res;
  }
}
}  // namespace

bool is_tsc_invariant(const CpuInfo& cpu) noexcept {
  if (cpu.has<hw::Feature::INVARIANT_TSC>()) return true;

  // Invariant TSC is masked by default for live-migration compatibility unless +invtsc is passed,
  // but both KVM and TCG back guest TSC with a monotonic clock.
  if (cpu.hypervisor() == Hypervisor::Kvm || cpu.hypervisor() == Hypervisor::Qemu) {
    return true;
  }

  // Any hypervisor advertising the standard 0x40000010 timing leaf provides a fixed-frequency
  // virtual TSC.
  if (cpu.is_virtualized()) {
    auto [max_hv, _, _, _] = cpu.query(0x40000000);

    if (max_hv >= 0x40000010) {
      auto [tsc_khz, _, _, _] = cpu.query(0x40000010);
      return tsc_khz != 0;
    }
  }

  return false;
}

Result probe_hardware_frequencies(const CpuInfo& cpu) noexcept {
  const Result hv = probe_hypervisor(cpu);
  if (hv.tsc_hz != 0 || hv.apic_bus_hz != 0) {
    return hv;
  }

  if (cpu.vendor() == Vendor::Intel) {
    return probe_intel(cpu);
  }

  if (cpu.vendor() == Vendor::Amd) {
    return probe_amd(cpu);
  }
  return {};
}
}  // namespace kernel::hw::timer