#pragma once

#include <bitfield.hpp>
#include <cstdint>

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
}  // namespace kernel::hw