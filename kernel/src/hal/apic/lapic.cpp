#include "hal/apic/lapic.hpp"

#include "hal/smp/cpu_info.hpp"

namespace kernel::hw::apic {
std::uint64_t Lapic::calibrate_bus_frequency() const noexcept {
  const std::uint64_t cached = s_calibrated_bus_hz.load(std::memory_order_acquire);
  if (cached != 0) {
    return cached;
  }

  std::uint64_t bus_hz = drivers::SystemClock::cached_apic_bus_hz();

  // Direct measurement against SystemClock.
  const std::uint64_t sys_hz = drivers::SystemClock::scaler().freq_hz;
  if (bus_hz == 0 && sys_hz != 0) {
    using namespace klib::literals;

    write(Reg::TimerDivide, Raw32Schema{std::to_underlying(TimerDivide::Div1)});
    write(Reg::LvtTimer, LvtSchema{}.with_timer_mode(TimerMode::OneShot).with_masked());

    // Measure over 3 x 1ms windows against SystemClock::raw_ticks()
    constexpr auto sample_time = (1_ms).to_ns();
    const std::uint64_t window_ticks =
        std::max(1ul, drivers::SystemClock::scaler().ns_to_ticks(sample_time));

    std::array<std::uint64_t, 3> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
      write(Reg::TimerInitCnt, Raw32Schema{0xFFFFFFFF});

      const std::uint64_t sys_start = drivers::SystemClock::raw_ticks();
      const std::uint32_t apic_start = get_timer_current_count();

      std::uint64_t sys_end = sys_start;
      while ((sys_end - sys_start) < window_ticks) {
        cpu_relax();
        sys_end = drivers::SystemClock::raw_ticks();
      }

      const std::uint32_t apic_end = get_timer_current_count();

      const __uint128_t numer = static_cast<__uint128_t>(apic_start - apic_end) * sys_hz;
      samples[i] = static_cast<std::uint64_t>(numer / (sys_end - sys_start));
    }

    write(Reg::TimerInitCnt, Raw32Schema{0});
    std::ranges::sort(samples);
    bus_hz = samples[1];
  }

  if (bus_hz != 0) {
    s_calibrated_bus_hz.store(bus_hz, std::memory_order_release);
  }

  return bus_hz;
}

void Lapic::initialize(memory::VirtualAddress mmio_base, bool prefer_x2apic) noexcept {
  m_mmio.set_base(mmio_base);

  const auto cpu = profile_manager.get_current();
  m_caps.has_x2apic = cpu->has<Feature::X2APIC>();
  m_caps.has_tsc_deadline = cpu->has<Feature::TSC_DEADLINE>();

  if (cpu->has<Feature::MCE>()) {
    constexpr std::uint32_t IA32_MCG_CAP = 0x179;
    const std::uint64_t mcg_cap = read::msr(IA32_MCG_CAP);
    m_caps.has_cmci = (mcg_cap & (1u << 10)) != 0;
  }

  m_caps.has_amd_ext_apic = cpu->has<Feature::EXT_APIC>();

  BaseMsrSchema apic_base = read::msr(IA32_APIC_BASE);
  apic_base.set_global_enable();

  if (apic_base.get_x2apic_enable() || (prefer_x2apic && m_caps.has_x2apic)) {
    apic_base.set_x2apic_enable();
    write::msr(IA32_APIC_BASE, apic_base.raw());

    m_mode = ApicMode::x2APIC;
    m_cached_id = read<IdSchema>(Reg::Id).get_x2apic_id();

    // Hardwired from the APIC ID: cluster in the upper half, one bit per CPU in the lower one.
    m_logical_id = read<LdrSchema>(Reg::Ldr).get_id();
  } else {
    write::msr(IA32_APIC_BASE, apic_base.raw());

    m_mode = ApicMode::xAPIC;
    m_cached_id = read<IdSchema>(Reg::Id).get_xapic_id();
    m_logical_id = 0;
  }

  const auto ver = read<VersionSchema>(Reg::Version);
  m_caps.max_lvt_entry = ver.get_max_lvt_entry();
  m_caps.has_directed_eoi = ver.get_has_directed_eoi();

  if (m_caps.has_amd_ext_apic) {
    m_caps.amd_ext_lvt_count = read<AmdExtFeatureSchema>(Reg::AmdExtFeature).get_ext_lvt_count();
  }

  // Accept all interrupts
  set_tpr(0);

  // Clear Error Status Register (Double-write for xAPIC)
  write(Reg::Esr, Raw32Schema{0});
  if (m_mode == ApicMode::xAPIC) {
    write(Reg::Esr, Raw32Schema{0});
  }

  auto mask_lvt = [this](const Reg reg) { write(reg, read<LvtSchema>(reg).with_masked()); };

  mask_lvt(Reg::LvtTimer);
  mask_lvt(Reg::LvtThermal);
  mask_lvt(Reg::LvtPerf);
  mask_lvt(Reg::LvtLint0);
  mask_lvt(Reg::LvtLint1);
  mask_lvt(Reg::LvtError);
  if (m_caps.has_cmci) {
    mask_lvt(Reg::LvtCmci);
  }

  for (std::uint8_t i = 0; i < m_caps.amd_ext_lvt_count; ++i) {
    mask_lvt(static_cast<Reg>(std::to_underlying(Reg::AmdExtLvtBase) + i * 0x10));
  }

  // Software enable
  const auto svr = SvrSchema{}.with_apic_enable().with_vector(0xFF);
  write(Reg::Svr, svr);

  // Configure TSC-Deadline preference and calibrate LAPIC bus frequency
  m_use_tsc_deadline = m_caps.has_tsc_deadline &&
                       drivers::SystemClock::backend() == drivers::ClockBackend::InvariantTsc;

  const std::uint64_t bus_hz = calibrate_bus_frequency();
  if (bus_hz != 0) {
    m_bus_scaler = drivers::FrequencyScaler{bus_hz};
  }
}
}  // namespace kernel::hw::apic