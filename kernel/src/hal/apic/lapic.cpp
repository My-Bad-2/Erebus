#include "hal/apic/lapic.hpp"
#include "hal/cpu_info.hpp"

namespace kernel::hw::apic {
void Lapic::initialize(memory::VirtualAddress mmio_base, bool prefer_x2apic) noexcept {
  m_mmio.set_base(mmio_base);

  const auto cpu = profile_manager.get_current();
  m_caps.has_x2apic = cpu->has<Feature::X2APIC>();
  m_caps.has_tsc_deadline = cpu->has<Feature::TSC_DEADLINE>();

  if (cpu->has<Feature::MCE>()) {
    constexpr std::uint32_t IA32_MCG_CAP = 0x179;
    const std::uint64_t mcg_cap = read::msr(IA32_MCG_CAP);

    m_caps.has_cmci = (mcg_cap & (1 << 10)) != 0;
  }

  m_caps.has_amd_ext_apic = cpu->has<Feature::EXT_APIC>();

  BaseMsrSchema apic_base = read::msr(IA32_APIC_BASE);
  apic_base.set_global_enable();

  if (prefer_x2apic && m_caps.has_x2apic) {
    apic_base.set_x2apic_enable();
    write::msr(IA32_APIC_BASE, apic_base.raw());

    m_mode = ApicMode::x2APIC;
    m_cached_id = read<IdSchema>(Reg::Id).get_x2apic_id();
  } else {
    apic_base.clear_x2apic_enable();
    write::msr(IA32_APIC_BASE, apic_base.raw());

    m_mode = ApicMode::x2APIC;
    m_cached_id = read<IdSchema>(Reg::Id).get_x2apic_id();
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
    mask_lvt(static_cast<Reg>(std::to_underlying(Reg::AmdExtLvtBase) + (i * 0x10)));
  }

  // Software enable
  const auto svr = SvrSchema{}.with_apic_enable().with_vector(0xFF);
  write(Reg::Svr, svr);
}
} // namespace kernel::hw::apic