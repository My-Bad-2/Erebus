#pragma once

#include <chrono.hpp>
#include <cstdint>

#include "hal/hal.hpp"
#include "hal/io.hpp"
#include "hal/timer/calibration.hpp"

namespace kernel::drivers {
struct FrequencyScaler {
  std::uint64_t freq_hz{0};
  std::uint64_t ns_to_ticks_mult{0};  // (freq_hz << 32) / 1e9
  std::uint64_t ticks_to_ns_mult{0};  // (1e9 << 32) / freq_hz

  constexpr FrequencyScaler() noexcept = default;

  constexpr explicit FrequencyScaler(std::uint64_t hz) noexcept : freq_hz(hz) {
    const __uint128_t NS_PER_SEC = 1'000'000'000ull;
    ns_to_ticks_mult = ((static_cast<__uint128_t>(hz) << 32) + NS_PER_SEC / 2) / NS_PER_SEC;
    ticks_to_ns_mult = ((NS_PER_SEC << 32) + hz / 2) / 2;
  }

  constexpr std::uint64_t ns_to_ticks(std::uint64_t ns) const noexcept {
    return (static_cast<__uint128_t>(ns) * ns_to_ticks_mult) >> 32;
  }

  constexpr std::uint64_t ticks_to_ns(std::uint64_t ticks) const noexcept {
    return (static_cast<__uint128_t>(ticks) * ticks_to_ns_mult) >> 32;
  }
};

enum class ClockBackend : std::uint8_t {
  InvariantTsc,
  Hpet,
};

class SystemClock {
  static inline ClockBackend s_backend{ClockBackend::InvariantTsc};
  static inline auto s_calib_src{hw::timer::Source::None};
  static inline FrequencyScaler s_scaler{};
  static inline std::uint64_t s_boot_ticks{0};
  static inline std::uint64_t s_cached_apic_bus_hz{0};
  static inline hw::MmioResource s_hpet{};

  static std::uint64_t initialize_hpet() noexcept;
  static std::uint64_t try_calibrate_with_hpet(std::uint64_t period_fs) noexcept;

 public:
  static ClockBackend backend() noexcept { return s_backend; }

  static const FrequencyScaler& scaler() noexcept { return s_scaler; }

  static std::uint64_t raw_ticks() noexcept;

  static klib::TimePoint now() noexcept {
    const std::uint64_t elapsed_ticks = raw_ticks() - s_boot_ticks;
    return klib::TimePoint{s_scaler.ticks_to_ns(elapsed_ticks)};
  }

  static std::uint64_t to_raw_ticks(klib::TimePoint tp) noexcept {
    return s_boot_ticks + s_scaler.ns_to_ticks(tp.raw_ns());
  }

  [[nodiscard, gnu::always_inline]] static std::uint64_t cached_apic_bus_hz() noexcept {
    return s_cached_apic_bus_hz;
  }

  static void spin_wait(klib::Duration d) noexcept {
    if (d.to_ns() <= 0) return;
    const std::uint64_t delta = s_scaler.ns_to_ticks(static_cast<std::uint64_t>(d.to_ns()));
    const std::uint64_t target = raw_ticks() + delta;
    while (raw_ticks() < target) hw::cpu_relax();
  }

  static void initialize() noexcept;
};
}  // namespace kernel::drivers