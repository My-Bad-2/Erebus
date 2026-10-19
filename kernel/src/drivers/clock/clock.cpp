#include "drivers/clock.hpp"

#include "hpet.hpp"
#include "utils/logger.hpp"

namespace kernel::drivers {
std::uint64_t SystemClock::raw_ticks() noexcept {
  if (s_backend == ClockBackend::InvariantTsc) [[likely]]
    return hw::read_tsc();
  return s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET);
}

void SystemClock::initialize() noexcept {
  const hw::CpuInfo& cpu = *hw::profile_manager.get_current();
  const bool tsc_usable = hw::timer::is_tsc_invariant(cpu);

  // Calibrate TSC via CPUID / MSRs
  if (tsc_usable) {
    const hw::timer::Result res = hw::timer::probe_hardware_frequencies(cpu);
    const std::uint64_t tsc_hz = res.tsc_hz;
    s_cached_apic_bus_hz = res.apic_bus_hz;

    if (tsc_hz != 0) {
      s_backend = ClockBackend::InvariantTsc;
      s_scaler = FrequencyScaler{tsc_hz};
      s_boot_ticks = hw::read_tsc();
      s_calib_src = res.source;
      return;
    }
  }

  // No CPUID/MSR option left. Enable HPET
  const std::uint64_t period_fs = initialize_hpet();
  if (period_fs == 0) {
    s_calib_src = hw::timer::Source::None;
    return;
  }

  // If TSC is invariant, measure its frequency via HPET and halt HPET
  if (tsc_usable) {
    const std::uint64_t tsc_hz = try_calibrate_with_hpet(period_fs);
    if (tsc_hz != 0) {
      const auto cfg = hpet::GeneralConfig{s_hpet.read<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET)};
      s_hpet.write<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET, cfg.without_enable_cnf().raw());

      s_backend = ClockBackend::InvariantTsc;
      s_scaler = FrequencyScaler{tsc_hz};
      s_boot_ticks = hw::read_tsc();
      return;
    }
  }

  // TSC is unusable. Keep HPET active as SystemClock
  const std::uint64_t hpet_hz = 1'000'000'000'000'000ul / period_fs;
  s_backend = ClockBackend::Hpet;
  s_calib_src = hw::timer::Source::HpetDirect;
  s_scaler = FrequencyScaler{hpet_hz};
  s_boot_ticks = s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET);
}
}  // namespace kernel::drivers