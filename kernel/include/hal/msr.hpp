#pragma once

#include <bitfield.hpp>
#include <cstdint>

#include "memory/address.hpp"

namespace kernel::hw {
inline constexpr std::uint32_t MSR_PLATFORM_INFO = 0xCE;
struct MsrPlatformInfo {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrPlatformInfo() noexcept = default;
  explicit constexpr MsrPlatformInfo(std::uint64_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RO(std::uint8_t, max_non_turbo_ratio, 8, 8);
  BF_BIT_RO(ppin_cap, 23);                // Protected Processor Inventory Number Capability
  BF_BIT_RO(prog_ratio_limit_turbo, 28);  // Programmable Ratio Limit for Turbo Mode
  BF_BIT_RO(prog_tdp_limit_turbo, 29);    // Programmable TDP Limit for Turbo Mode
  BF_BIT_RO(prog_tj_offset, 30);          // Programmable TJ Offset
  BF_RO(std::uint8_t, max_efficiency_ratio, 40, 8);
};

inline constexpr std::uint32_t MSR_AMD_NB_CFG = 0xC001001F;
// AMD Family 10h/15h/16h BKDG: MSRC001_001F [Northbridge Configuration (NB_CFG)]
// Bit 46 (EnableCf8ExtCfg): Enables PCI extended config space [0x100..0xFFF] via 0xCF8
struct AmdNbCfgMsr {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr AmdNbCfgMsr() noexcept = default;
  constexpr AmdNbCfgMsr(std::uint64_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(dis_io_req_lock, 3);
  BF_BIT_RW(dis_dat_msk, 36);
  BF_BIT_RW(enable_cf8_ext_cfg, 46);
  BF_BIT_RW(init_apic_id_cpu_id_lo, 54);
};

inline constexpr std::uint32_t MSR_AMD_CPB_CONTROL = 0x15C;
// AMD Family 10h/15h/16h BKDG: D18F4x15C [Core Performance Boost Control]
// Fam10h: Bit [2] = NumBoostStates; Fam12h/15h/16h: Bits [4:2] = NumBoostStates
struct AmdCpbControlReg {
 private:
  std::uint32_t m_data{0};

 public:
  constexpr AmdCpbControlReg() noexcept = default;
  explicit constexpr AmdCpbControlReg(std::uint32_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, num_boost_states, 0, 3);
  BF_RW(std::uint8_t, boost_src, 3, 2);
  BF_BIT_RW(apm_master_en, 7);
  BF_BIT_RW(boost_lock, 31);
};

inline constexpr std::uint32_t MSR_AMD_PSTATE_DEF_BASE = 0xC0010064;
// AMD Family 10h (#31116), Family 15h (#42301), Family 16h (#48751): MSRC001_00[6B:64]
// COF = (100 MHz * (CpuFid + 10h)) / (2^CpuDid)
struct MsrLegacyPStateP0 {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrLegacyPStateP0() noexcept = default;
  explicit constexpr MsrLegacyPStateP0(std::uint64_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, cpu_fid, 0, 6);
  BF_RW(std::uint8_t, cpu_did, 6, 3);
  BF_RW(std::uint8_t, cpu_vid, 9, 7);
  BF_RW(std::uint8_t, idd_value, 16, 6);
  BF_RW(std::uint8_t, idd_div, 22, 3);
  BF_BIT_RW(pstate_en, 63);
};

// AMD Family 17h OSRR (#56255) & Family 19h PPR (#55898) [Zen 1 - Zen 4]: MSRC001_0064
// COF = (CpuFid * 200 MHz) / CpuDfsId
struct MsrZenPStateP0 {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrZenPStateP0() noexcept = default;
  explicit constexpr MsrZenPStateP0(std::uint64_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, cpu_fid, 0, 8);
  BF_RW(std::uint8_t, cpu_dfs_id, 8, 6);
  BF_RW(std::uint8_t, cpu_vid, 14, 8);
  BF_RW(std::uint8_t, idd_value, 22, 6);
  BF_RW(std::uint8_t, idd_div, 28, 2);
  BF_BIT_RW(pstate_en, 63);
};

// AMD Family 1Ah PPR (#57095) [Zen 5]: MSRC001_0064
// COF = CpuFid * 5 MHz
struct MsrZen5PStateP0 {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrZen5PStateP0() noexcept = default;
  explicit constexpr MsrZen5PStateP0(std::uint64_t data) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint16_t, cpu_fid, 0, 12);
  BF_RW(std::uint8_t, cpu_vid, 14, 8);
  BF_RW(std::uint8_t, idd_value, 22, 6);
  BF_RW(std::uint8_t, idd_div, 28, 2);
  BF_BIT_RW(pstate_en, 63);
};

// SYSCALL and SYSRET. The AMD64 manual, volume 2, section 6.1.1, and the SDM, volume 3, section
// 5.8.8, say what each of these does.

/// @brief IA32_STAR: the selectors SYSCALL and SYSRET load. Each instruction takes one base and
///        finds the rest of what it needs in the GDT entries right after it, so the GDT has to be
///        laid out the way gdt::Selector is.
inline constexpr std::uint32_t MSR_IA32_STAR = 0xC0000081;
struct MsrSyscallStar {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrSyscallStar() noexcept = default;
  constexpr explicit MsrSyscallStar(const std::uint64_t data) noexcept : m_data{data} {}

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  /// SYSCALL loads CS from this and SS from this + 8. FRED event delivery to ring 0 does too.
  BF_RW(std::uint16_t, syscall_base, 32, 16)
  /// SYSRET to 64-bit code loads CS from this + 16 and SS from this + 8, both with RPL 3.
  BF_RW(std::uint16_t, sysret_base, 48, 16)
};

/// @brief IA32_LSTAR, where SYSCALL from 64-bit code goes, and IA32_CSTAR, where it goes from
///        compatibility mode, which only AMD CPUs have.
inline constexpr std::uint32_t MSR_IA32_LSTAR = 0xC0000082;
inline constexpr std::uint32_t MSR_IA32_CSTAR = 0xC0000083;
struct MsrSyscallTarget {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrSyscallTarget() noexcept = default;
  constexpr explicit MsrSyscallTarget(const memory::VirtualAddress entry) noexcept
      : m_data{entry.value()} {}

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }
  [[nodiscard]] constexpr memory::VirtualAddress entry() const noexcept {
    return memory::VirtualAddress{m_data};
  }
};

/// @brief IA32_FMASK: the RFLAGS bits SYSCALL clears, after saving RFLAGS in R11.
inline constexpr std::uint32_t MSR_IA32_FMASK = 0xC0000084;
struct MsrSyscallFlagMask {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr MsrSyscallFlagMask() noexcept = default;
  constexpr explicit MsrSyscallFlagMask(const std::uint64_t data) noexcept : m_data{data} {}

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, flag_mask, 0, 32)
};

/// @brief SYSENTER's code selector, stack and entry point. A null selector makes SYSENTER raise
///        #GP instead of going anywhere.
inline constexpr std::uint32_t MSR_IA32_SYSENTER_CS = 0x174;
inline constexpr std::uint32_t MSR_IA32_SYSENTER_ESP = 0x175;
inline constexpr std::uint32_t MSR_IA32_SYSENTER_EIP = 0x176;
}  // namespace kernel::hw
