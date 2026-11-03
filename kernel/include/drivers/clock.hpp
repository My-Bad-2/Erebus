/// @file clock.hpp
/// @brief The kernel's monotonic clock.
///
/// Time comes from the invariant TSC whenever there is one, since it is cheap to read and runs
/// at the same rate on every CPU. Its frequency is taken from CPUID or MSRs when the CPU reports
/// it, and measured against the HPET or the ACPI PM timer when it does not. Without an
/// invariant TSC the HPET, or failing that the PM timer, keeps time directly.

#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "hal/hal.hpp"
#include "hal/io.hpp"
#include "hal/timer/calibration.hpp"

namespace kernel::drivers {
/// @brief Converts between ticks of a fixed-frequency counter and nanoseconds with one 128-bit
///        multiply and a shift (32.32 fixed point), no division.
struct FrequencyScaler {
  std::uint64_t freq_hz{0};
  std::uint64_t ns_to_ticks_mult{0};  // (freq_hz << 32) / 1e9
  std::uint64_t ticks_to_ns_mult{0};  // (1e9 << 32) / freq_hz

  constexpr FrequencyScaler() noexcept = default;

  constexpr explicit FrequencyScaler(std::uint64_t hz) noexcept : freq_hz(hz) {
    const __uint128_t NS_PER_SEC = 1'000'000'000ull;
    ns_to_ticks_mult = ((static_cast<__uint128_t>(hz) << 32) + NS_PER_SEC / 2) / NS_PER_SEC;
    ticks_to_ns_mult = hz != 0 ? ((NS_PER_SEC << 32) + hz / 2) / hz : 0;
  }

  constexpr std::uint64_t ns_to_ticks(std::uint64_t ns) const noexcept {
    return (static_cast<__uint128_t>(ns) * ns_to_ticks_mult) >> 32;
  }

  constexpr std::uint64_t ticks_to_ns(std::uint64_t ticks) const noexcept {
    return (static_cast<__uint128_t>(ticks) * ticks_to_ns_mult) >> 32;
  }
};

enum class ClockBackend : std::uint8_t {
  InvariantTsc,  ///< RDTSC, at a constant rate on every CPU.
  Hpet,          ///< HPET main counter.
  AcpiPmTimer,   ///< ACPI PM timer: slow to read, and only 24 bits wide on older chipsets.
  None,          ///< Nothing usable was found; time does not advance.
};

class SystemClock {
  static inline auto s_backend{ClockBackend::InvariantTsc};
  static inline auto s_calib_src{hw::timer::Source::None};
  static inline FrequencyScaler s_scaler{};
  static inline std::uint64_t s_boot_ticks{0};
  static inline std::uint64_t s_cached_apic_bus_hz{0};

  static inline hw::MmioResource s_hpet{};
  static inline std::uint64_t s_hpet_mask{~0ull};
  static inline std::uint16_t s_pm_timer_port{0};
  static inline std::uint64_t s_pm_timer_mask{0};

  // A counter narrower than 64 bits (a 32-bit HPET, the PM timer) wraps, so raw_ticks() widens
  // it: this holds the last value handed out, and each read adds how far the hardware moved
  // since. That only works if the clock is read at least once per half wrap period (about two
  // minutes for a 32-bit HPET, two seconds for a 24-bit PM timer).
  static inline std::uint64_t s_backend_mask{~0ull};
  static inline std::atomic<std::uint64_t> s_widened{0};

  [[nodiscard]] static std::uint64_t initialize_hpet() noexcept;
  [[nodiscard]] static bool initialize_pm_timer() noexcept;
  [[nodiscard]] static std::uint64_t read_hpet() noexcept;
  [[nodiscard]] static std::uint64_t read_pm_timer() noexcept;
  [[nodiscard]] static std::uint64_t widen(std::uint64_t raw) noexcept;
  static void use_backend(ClockBackend backend, std::uint64_t hz, hw::timer::Source source,
                          std::uint64_t mask) noexcept;

 public:
  [[nodiscard]] static ClockBackend backend() noexcept { return s_backend; }
  [[nodiscard]] static hw::timer::Source source() noexcept { return s_calib_src; }

  [[nodiscard]] static const FrequencyScaler& scaler() noexcept { return s_scaler; }

  /// @brief The backend's counter, widened to 64 bits. Never goes backwards on one CPU.
  [[nodiscard]] static std::uint64_t raw_ticks() noexcept {
    if (s_backend == ClockBackend::InvariantTsc) [[likely]] {
      return hw::read_tsc();
    }

    return raw_ticks_slow();
  }

  [[nodiscard]] static std::uint64_t raw_ticks_slow() noexcept;

  /// @brief Time since boot.
  [[nodiscard]] static klib::TimePoint now() noexcept {
    // A CPU whose TSC trails the BSP's by a few ticks must not see time before boot.
    const auto elapsed = static_cast<std::int64_t>(raw_ticks() - s_boot_ticks);
    return klib::TimePoint{
        s_scaler.ticks_to_ns(elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0)};
  }

  [[nodiscard]] static std::uint64_t to_raw_ticks(klib::TimePoint tp) noexcept {
    return s_boot_ticks + s_scaler.ns_to_ticks(tp.raw_ns());
  }

  [[nodiscard, gnu::always_inline]] static std::uint64_t cached_apic_bus_hz() noexcept {
    return s_cached_apic_bus_hz;
  }

  /// @brief Busy-waits for @p d. Returns at once if there is no working clock.
  static void spin_wait(klib::Duration d) noexcept {
    if (d.to_ns() <= 0 || s_scaler.freq_hz == 0) return;
    const std::uint64_t delta = s_scaler.ns_to_ticks(static_cast<std::uint64_t>(d.to_ns()));
    const std::uint64_t start = raw_ticks();
    while (raw_ticks() - start < delta) hw::cpu_relax();
  }

  static void initialize() noexcept;
};
}  // namespace kernel::drivers
