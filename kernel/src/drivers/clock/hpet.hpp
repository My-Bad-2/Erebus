#pragma once

#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

namespace kernel::drivers::hpet {
constexpr std::size_t GENERAL_CAP_AND_ID_OFFSET = 0x0;
constexpr std::size_t GENERAL_CONFIG_OFFSET = 0x10;
constexpr std::size_t GENERAL_INTR_STATUS_OFFSET = 0x20;
constexpr std::size_t MAIN_COUNTER_OFFSET = 0xF0;
constexpr std::size_t TIMER0_CONFIG_OFFSET = 0x100;
constexpr std::size_t TIMER0_COMPARATOR_OFFSET = 0x108;

struct GeneralCap {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr explicit GeneralCap(std::uint64_t data = 0) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_RO(std::uint8_t, rev_id, 0, 8)
  BF_RO(std::uint8_t, num_timers_minus_one, 8, 5)
  BF_BIT_RO(counter_64bit_cap, 13)
  BF_BIT_RO(legacy_replacement_cap, 15)
  BF_RO(std::uint16_t, vendor_id, 16, 16)
  BF_RO(std::uint32_t, counter_clk_period_fs, 32, 32)
};

struct GeneralConfig {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr explicit GeneralConfig(std::uint64_t data = 0) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(enable_cnf, 0)
  BF_BIT_RW(legacy_replacement_cnf, 1)
};

struct GeneralIntrStatus {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr explicit GeneralIntrStatus(std::uint64_t data = 0) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_W1C(t0_int_sts, 0)
  BF_BIT_W1C(t1_int_sts, 1)
  BF_BIT_W1C(t2_int_sts, 2)
};

// Offset 0x100 + (0x20 * N): Timer N Configuration and Capability Register
struct TimerConfig {
 private:
  std::uint64_t m_data{0};

 public:
  constexpr explicit TimerConfig(std::uint64_t data = 0) noexcept : m_data{data} {}

  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(level_triggered_cnf, 1)
  BF_BIT_RW(int_enable_cnf, 2)
  BF_BIT_RW(periodic_cnf, 3)
  BF_BIT_RO(periodic_cap, 4)
  BF_BIT_RO(size_64bit_cap, 5)
  BF_BIT_RW(val_set_cnf, 6)
  BF_BIT_RW(mode32_cnf, 8)
  BF_RW(std::uint8_t, ioapic_route_cnf, 9, 5)
  BF_BIT_RW(fsb_enable_cnf, 14)
  BF_BIT_RO(fsb_delivery_cap, 15)
  BF_RO(std::uint32_t, ioapic_route_cap, 32, 32)
};
}  // namespace kernel::drivers::hpet