#include "drivers/clock.hpp"

#include <algorithm>
#include <array>

#include "utils/logger.hpp"

namespace kernel::drivers {
namespace {
constexpr std::uint64_t FS_PER_SEC = 1'000'000'000'000'000ul;
constexpr std::uint64_t PM_TIMER_HZ = 3'579'545;

// A reference counter to measure the TSC against. Its frequency is `hz_num / hz_den` Hz, which
// lets the HPET pass its period in femtoseconds without rounding.
struct Reference {
  std::uint64_t (*read)() noexcept;
  std::uint64_t mask;
  std::uint64_t hz_num;
  std::uint64_t hz_den;
};

// Three 10 ms windows, median taken. Each window gives up after a bounded number of reads, so
// a reference counter that does not count fails the calibration instead of hanging the boot.
// A window takes some 10-20 thousand reads on real hardware; the limit leaves plenty of room.
std::uint64_t calibrate_tsc(const Reference& ref) noexcept {
  constexpr std::uint64_t WINDOWS_PER_SEC = 100;
  constexpr std::uint64_t READ_LIMIT = 2'000'000;

  const std::uint64_t window_ticks =
      std::max<std::uint64_t>(1, ref.hz_num / (ref.hz_den * WINDOWS_PER_SEC));
  std::array<std::uint64_t, 3> samples{};

  for (std::uint64_t& sample : samples) {
    std::uint64_t reads = 0;

    // Start on a tick edge, so the window is not short by a fraction of a tick.
    const std::uint64_t initial = ref.read();
    std::uint64_t start = initial;
    while (start == initial) {
      if (++reads > READ_LIMIT) [[unlikely]] {
        return 0;
      }

      start = ref.read();
    }

    const std::uint64_t tsc_start = hw::read_tsc();

    std::uint64_t ticks = 0;
    while (ticks < window_ticks) {
      if (++reads > READ_LIMIT) [[unlikely]] {
        return 0;
      }

      ticks = (ref.read() - start) & ref.mask;
    }

    const std::uint64_t tsc_ticks = hw::read_tsc() - tsc_start;

    const __uint128_t numer = static_cast<__uint128_t>(tsc_ticks) * ref.hz_num;
    const __uint128_t denom = static_cast<__uint128_t>(ticks) * ref.hz_den;
    sample = static_cast<std::uint64_t>(numer / denom);
  }

  std::ranges::sort(samples);
  return samples[1];
}

[[nodiscard]] constexpr const char* to_string(const hw::timer::Source source) noexcept {
  using hw::timer::Source;
  switch (source) {
    case Source::HypervisorLeaf:
      return "hypervisor CPUID leaf";
    case Source::IntelCpuid15:
      return "CPUID leaf 0x15";
    case Source::IntelCpuid16:
      return "CPUID leaf 0x16";
    case Source::IntelMsrPlatformInfo:
      return "MSR_PLATFORM_INFO";
    case Source::AmdMsrPstateDef:
      return "P-state MSR";
    case Source::HpetSoftware:
      return "measured against the HPET";
    case Source::HpetDirect:
      return "HPET";
    case Source::PmTimerSoftware:
      return "measured against the ACPI PM timer";
    case Source::PmTimerDirect:
      return "ACPI PM timer";
    default:
      return "unknown";
  }
}

[[nodiscard]] constexpr const char* to_string(const ClockBackend backend) noexcept {
  switch (backend) {
    case ClockBackend::InvariantTsc:
      return "TSC";
    case ClockBackend::Hpet:
      return "HPET";
    case ClockBackend::AcpiPmTimer:
      return "ACPI PM timer";
    default:
      return "none";
  }
}
}  // namespace

std::uint64_t SystemClock::widen(const std::uint64_t raw) noexcept {
  std::uint64_t last = s_widened.load(std::memory_order_relaxed);

  while (true) {
    // How far the hardware moved since the last value anyone handed out. A reading that looks
    // like it went backwards came from a CPU that raced a later one; it gets the later value.
    const std::uint64_t delta = (raw - last) & s_backend_mask;
    if (delta > (s_backend_mask >> 1)) {
      return last;
    }

    if (s_widened.compare_exchange_weak(last, last + delta, std::memory_order_relaxed)) {
      return last + delta;
    }
  }
}

std::uint64_t SystemClock::raw_ticks_slow() noexcept {
  switch (s_backend) {
    case ClockBackend::Hpet:
      return s_backend_mask == ~0ul ? read_hpet() : widen(read_hpet());
    case ClockBackend::AcpiPmTimer:
      return widen(read_pm_timer());
    default:
      // No clock: the TSC still orders events, even if nobody knows how fast it runs.
      return hw::read_tsc();
  }
}

void SystemClock::use_backend(const ClockBackend backend, const std::uint64_t hz,
                              const hw::timer::Source source, const std::uint64_t mask) noexcept {
  s_backend = backend;
  s_calib_src = source;
  s_scaler = FrequencyScaler{hz};
  s_backend_mask = mask;

  if (backend == ClockBackend::Hpet) {
    s_widened.store(read_hpet(), std::memory_order_relaxed);
  } else if (backend == ClockBackend::AcpiPmTimer) {
    s_widened.store(read_pm_timer(), std::memory_order_relaxed);
  }

  s_boot_ticks = raw_ticks();
}

void SystemClock::initialize() noexcept {
  const hw::CpuInfo& cpu = *hw::profile_manager.get_current();
  const bool tsc_usable = hw::timer::is_tsc_invariant(cpu);

  // Cheapest first: the CPU or the hypervisor may simply say how fast the TSC runs.
  if (tsc_usable) {
    const hw::timer::Result res = hw::timer::probe_hardware_frequencies(cpu);
    s_cached_apic_bus_hz = res.apic_bus_hz;

    if (res.tsc_hz != 0) {
      use_backend(ClockBackend::InvariantTsc, res.tsc_hz, res.source, ~0ul);
    }
  }

  if (s_scaler.freq_hz == 0) {
    const std::uint64_t hpet_period_fs = initialize_hpet();
    const bool pm_timer = initialize_pm_timer();

    const Reference hpet_ref{&read_hpet, s_hpet_mask, FS_PER_SEC, hpet_period_fs};
    const Reference pm_ref{&read_pm_timer, s_pm_timer_mask, PM_TIMER_HZ, 1};

    if (tsc_usable) {
      // Measure the TSC against whatever reference there is. The HPET is far quicker to read.
      std::uint64_t tsc_hz = hpet_period_fs != 0 ? calibrate_tsc(hpet_ref) : 0;
      auto source = hw::timer::Source::HpetSoftware;

      if (tsc_hz == 0 && pm_timer) {
        tsc_hz = calibrate_tsc(pm_ref);
        source = hw::timer::Source::PmTimerSoftware;
      }

      if (tsc_hz != 0) {
        use_backend(ClockBackend::InvariantTsc, tsc_hz, source, ~0ul);
      }
    }

    // The TSC cannot be trusted: keep time with the reference directly.
    if (s_scaler.freq_hz == 0 && hpet_period_fs != 0) {
      use_backend(ClockBackend::Hpet, FS_PER_SEC / hpet_period_fs, hw::timer::Source::HpetDirect,
                  s_hpet_mask);
    } else if (s_scaler.freq_hz == 0 && pm_timer) {
      use_backend(ClockBackend::AcpiPmTimer, PM_TIMER_HZ, hw::timer::Source::PmTimerDirect,
                  s_pm_timer_mask);
    }
  }

  if (s_scaler.freq_hz == 0) [[unlikely]] {
    s_backend = ClockBackend::None;
    utils::logger::error("Clock: no usable time source; timeouts will not work.\n");
    return;
  }

  utils::logger::info("Clock: {} at {} kHz ({})\n", to_string(s_backend), s_scaler.freq_hz / 1000,
                      to_string(s_calib_src));
}
}  // namespace kernel::drivers
