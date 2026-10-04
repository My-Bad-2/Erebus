#pragma once

#include "apic.hpp"
#include "hal/hal.hpp"
#include "hal/io.hpp"
#include "memory/address.hpp"
#include "utility"

#include <bitfield.hpp>

namespace kernel::hw::apic {
constexpr std::uint32_t IA32_APIC_BASE = 0x1B;

enum class Reg : std::uint32_t {
  Id = 0x0020,
  Version = 0x0030,
  Tpr = 0x0080,
  Ppr = 0x00A0,
  Eoi = 0x00B0,
  Ldr = 0x00D0,
  Dfr = 0x00E0,
  Svr = 0x00F0,
  Esr = 0x0280,
  LvtCmci = 0x02F0, // CMCI: Corrected Machine Check Interrupt
  IcrLow = 0x0300,
  IcrHigh = 0x0310,
  LvtTimer = 0x0320,
  LvtThermal = 0x0330,
  LvtPerf = 0x0340,
  LvtLint0 = 0x0350,
  LvtLint1 = 0x0360,
  LvtError = 0x0370,
  TimerInitCnt = 0x0380,
  TimerCurrCnt = 0x0390,
  TimerDivide = 0x03E0,
  AmdExtFeature = 0x0400,
  AmdExtControl = 0x0410,
  AmdExtLvtBase = 0x0500 // Base + (0x10 * index) up to 4
};

struct BaseMsrSchema {
private:
  std::uint64_t m_data{0};

public:
  BaseMsrSchema(const std::uint64_t data = 0) noexcept : m_data{data} {}
  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(bsp, 8)
  BF_BIT_RW(x2apic_enable, 10)
  BF_BIT_RW(global_enable, 11)
  BF_RW(std::uint64_t, base_address, 12, 40)
};

struct IdSchema {
private:
  std::uint32_t m_data{0};

public:
  IdSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, xapic_id, 24, 8)
  BF_RW(std::uint32_t, x2apic_id, 0, 32)
};

struct VersionSchema {
private:
  std::uint32_t m_data{0};

public:
  VersionSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, version, 0, 8)
  BF_RW(std::uint8_t, max_lvt_entry, 16, 8)
  BF_BIT_RO(has_directed_eoi, 24)
};

struct LdrSchema {
private:
  std::uint32_t m_data{0};

public:
  LdrSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, id, 0, 32)
};

struct DfrSchema {
private:
  std::uint32_t m_data{0};

public:
  DfrSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, model, 28, 4) // 0xF = Flat, 0x0 = Cluster
};

struct SvrSchema {
private:
  std::uint32_t m_data{0};

public:
  SvrSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, vector, 0, 8)
  BF_BIT_RW(apic_enable, 8)
  BF_BIT_RW(eoi_suppress, 12)
};

struct LvtSchema {
private:
  std::uint32_t m_data{0};

public:
  LvtSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, vector, 0, 8)
  BF_RW(DeliveryMode, delivery_mode, 8, 3)
  BF_BIT_RO(delivery_status, 12)
  BF_RW(PinPolarity, polarity, 13, 1)
  BF_BIT_RO(remote_irr, 14)
  BF_RW(TriggerMode, trigger_mode, 15, 1)
  BF_BIT_RW(masked, 16)
  BF_RW(TimerMode, timer_mode, 17, 2)
};

struct IcrSchema {
private:
  std::uint64_t m_data{0};

public:
  IcrSchema(const std::uint64_t data = 0) noexcept : m_data{data} {}
  std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, vector, 0, 8)
  BF_RW(DeliveryMode, delivery_mode, 8, 3)
  BF_RW(DestinationMode, dest_mode, 11, 1)
  BF_BIT_RO(delivery_status, 12)
  BF_BIT_RW(level_assert, 14)
  BF_RW(TriggerMode, trigger_mode, 15, 1)
  BF_RW(DestinationShorthand, shorthand, 18, 2)
  BF_RW(std::uint32_t, destination, 32, 32)
};

struct PrioritySchema {
private:
  std::uint32_t m_data{0};

public:
  PrioritySchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, priority, 0, 8)
};

struct AmdExtFeatureSchema {
private:
  std::uint32_t m_data{0};

public:
  AmdExtFeatureSchema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_BIT_RO(ier_cap, 0)
  BF_BIT_RO(seoi_cap, 1)
  BF_BIT_RO(ext_apic_id_cap, 2)
  BF_RW(std::uint8_t, ext_lvt_count, 16, 8)
};

struct Raw32Schema {
private:
  std::uint32_t m_data{0};

public:
  Raw32Schema(const std::uint32_t data = 0) noexcept : m_data{data} {}
  std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, value, 0, 32)
};

class Lapic {
public:
  struct Capabilities {
    bool has_x2apic = false;
    bool has_tsc_deadline = false;
    bool has_cmci = false;
    bool has_directed_eoi = false;
    bool has_amd_ext_apic = false;
    std::uint8_t max_lvt_entry = 0;
    std::uint8_t amd_ext_lvt_count = 0;
  };

private:
  MmioResource m_mmio{};
  std::uint32_t m_cached_id{0};
  ApicMode m_mode{ApicMode::Disabled};
  Capabilities m_caps{};

  static constexpr std::uint32_t LAPIC_BASE_MSR = 0x800;

  void dispatch_icr(const IcrSchema icr) const noexcept {
    if (m_mode == ApicMode::x2APIC) [[likely]] {
      constexpr std::uint32_t IA32_X2APIC_ICR = 0x830;
      write::msr(IA32_X2APIC_ICR, icr.raw());
    } else {
      constexpr std::uint32_t icr_low = std::to_underlying(Reg::IcrLow);
      constexpr std::uint32_t icr_high = std::to_underlying(Reg::IcrHigh);

      IcrSchema icr_lo = m_mmio.read<std::uint32_t>(icr_low);
      while (icr_lo.get_delivery_status()) {
        cpu_relax();
        icr_lo = m_mmio.read<std::uint32_t>(icr_low);
      }

      m_mmio.write<std::uint32_t>(icr_high, static_cast<std::uint32_t>(icr.raw() >> 32));
      m_mmio.write<std::uint32_t>(icr_low, static_cast<std::uint32_t>(icr.raw()));

      icr_lo = m_mmio.read<std::uint32_t>(icr_low);
      while (icr_lo.get_delivery_status()) {
        cpu_relax();
        icr_lo = m_mmio.read<std::uint32_t>(icr_low);
      }
    }
  }

public:
  Lapic() = default;
  Lapic(const Lapic &) = delete;

  void initialize(memory::VirtualAddress mmio_base, bool prefer_x2apic = true) noexcept;

  [[nodiscard]] ApicMode mode() const noexcept { return m_mode; }
  [[nodiscard]] uint32_t id() const noexcept { return m_cached_id; }
  [[nodiscard]] const Capabilities &capabilities() const noexcept { return m_caps; }
  [[nodiscard]] static bool is_bsp() noexcept { return BaseMsrSchema{read::msr(IA32_APIC_BASE)}.get_bsp(); }

  template <typename Schema> [[nodiscard]] Schema read(const Reg reg) const noexcept {
    const std::uint32_t offset = std::to_underlying(reg);
    if (m_mode == ApicMode::x2APIC) [[likely]] {
      return Schema(static_cast<std::uint32_t>(read::msr(LAPIC_BASE_MSR + (offset >> 4))));
    }

    return Schema(m_mmio.read<std::uint32_t>(offset));
  }

  template <typename Schema> void write(const Reg reg, const Schema val) const noexcept {
    const std::uint32_t offset = std::to_underlying(reg);
    if (m_mode == ApicMode::x2APIC) [[likely]] {
      write::msr(LAPIC_BASE_MSR + (offset >> 4), val.raw());
    } else {
      m_mmio.write<std::uint32_t>(offset, val.raw());
    }
  }

  [[gnu::always_inline]] void eoi() const noexcept { write(Reg::Eoi, Raw32Schema{0}); }

  void enable_directed_eoi(const bool enable) const noexcept {
    if (m_caps.has_directed_eoi) {
      write(Reg::Svr, read<SvrSchema>(Reg::Svr).with_eoi_suppress(enable));
    }
  }

  void set_tpr(const std::uint8_t priority) const noexcept {
    write(Reg::Tpr, PrioritySchema{}.with_priority(priority));
  }

  [[nodiscard]] std::uint8_t get_tpr() const noexcept { return read<PrioritySchema>(Reg::Tpr).get_priority(); }
  [[nodiscard]] std::uint8_t get_ppr() const noexcept { return read<PrioritySchema>(Reg::Ppr).get_priority(); }

  void configure_logical_destination(const std::uint32_t logical_id,
                                     const std::uint8_t flat_model = 0xF) const noexcept {
    if (m_mode == ApicMode::xAPIC) {
      write(Reg::Dfr, DfrSchema{}.with_model(flat_model));
      write(Reg::Ldr, LdrSchema{}.with_id(logical_id << 24));
    }

    // In x2APIC, LDR is hardwired based on CPU Topology. DFR doesn't exist.
  }

  void disarm_timer() const noexcept {
    write(Reg::LvtTimer, read<LvtSchema>(Reg::LvtTimer).with_masked());
    write(Reg::TimerInitCnt, Raw32Schema{0});
  }

  void arm_periodic(const std::uint8_t vector, const std::uint32_t count, const TimerDivide div) const noexcept {
    const auto lvt = LvtSchema{}.with_vector(vector).with_timer_mode(TimerMode::Periodic);

    write(Reg::TimerDivide, Raw32Schema{std::to_underlying(div)});
    write(Reg::LvtTimer, lvt);
    write(Reg::TimerInitCnt, Raw32Schema{count});
  }

  void arm_oneshot(const std::uint8_t vector, const std::uint32_t count, const TimerDivide div) const noexcept {
    const auto lvt = LvtSchema{}.with_vector(vector).with_timer_mode(TimerMode::OneShot);

    write(Reg::TimerDivide, Raw32Schema{std::to_underlying(div)});
    write(Reg::LvtTimer, lvt);
    write(Reg::TimerInitCnt, Raw32Schema{count});
  }

  [[gnu::always_inline]] void arm_tsc_deadline(std::uint8_t vector, std::uint64_t target_tsc) const noexcept {
    const auto lvt = LvtSchema{}.with_vector(vector).with_timer_mode(TimerMode::TscDeadline);
    write(Reg::LvtTimer, lvt);
    asm volatile("mfence" : /* No Output */ : /* No Input */ : "memory");
    constexpr std::uint32_t TSC_DEADLINE_MSR = 0x6E0;
    write::msr(TSC_DEADLINE_MSR, target_tsc);
  }

  [[nodiscard]] std::uint32_t get_timer_current_count() const noexcept {
    return read<Raw32Schema>(Reg::TimerCurrCnt).get_value();
  }

  void configure_nmi(const Reg lint_reg = Reg::LvtLint1, const bool masked = false) const noexcept {
    write(lint_reg, LvtSchema{}.with_delivery_mode(DeliveryMode::NMI).with_masked(masked));
  }

  void configure_pmu(const std::uint8_t vector, const DeliveryMode delivery = DeliveryMode::Fixed,
                     const bool masked = false) const noexcept {
    write(Reg::LvtPerf, LvtSchema{}.with_vector(vector).with_delivery_mode(delivery).with_masked(masked));
  }

  void configure_cmci(const std::uint8_t vector, const DeliveryMode delivery = DeliveryMode::Fixed,
                      const bool masked = false) const noexcept {
    if (m_caps.has_cmci) {
      write(Reg::LvtCmci, LvtSchema{}.with_vector(vector).with_delivery_mode(delivery).with_masked(masked));
    }
  }

  void configure_amd_ext_lvt(const std::uint8_t index, const std::uint8_t vector,
                             const DeliveryMode delivery = DeliveryMode::Fixed,
                             const bool masked = false) const noexcept {
    if (!m_caps.has_amd_ext_apic || index >= m_caps.amd_ext_lvt_count) {
      return;
    }

    const auto target_reg = static_cast<Reg>(std::to_underlying(Reg::AmdExtLvtBase) + (index * 0x10));
    const auto lvt = LvtSchema{}.with_vector(vector).with_delivery_mode(delivery).with_masked(masked);
    write(target_reg, lvt);
  }

  void send_ipi(const std::uint32_t dest_id, const std::uint8_t vector,
                const DeliveryMode delivery = DeliveryMode::Fixed,
                const DestinationMode dest_mode = DestinationMode::Physical) const noexcept {
    const auto icr = IcrSchema{}
                         .with_vector(vector)
                         .with_delivery_mode(delivery)
                         .with_dest_mode(dest_mode)
                         .with_level_assert()
                         .with_destination(dest_id);
    dispatch_icr(icr);
  }

  [[gnu::always_inline]] void send_self_ipi(const std::uint8_t vector) const noexcept {
    if (m_mode == ApicMode::x2APIC) {
      constexpr std::uint32_t IA32_X2APIC_SELF_IPI = 0x83F;
      write::msr(IA32_X2APIC_SELF_IPI, vector);
    } else {
      const auto icr = IcrSchema{}.with_vector(vector).with_shorthand(DestinationShorthand::Self).with_level_assert();
      dispatch_icr(icr);
    }
  }

  void send_init(const std::uint32_t dest_id) const noexcept {
    const auto icr = IcrSchema{}
                         .with_delivery_mode(DeliveryMode::INIT)
                         .with_level_assert()
                         .with_trigger_mode(TriggerMode::Level)
                         .with_destination(dest_id);
    dispatch_icr(icr);
  }

  void send_sipi(const std::uint32_t dest_id, const std::uint8_t page_frame) const noexcept {
    const auto icr = IcrSchema{}
                         .with_vector(page_frame)
                         .with_delivery_mode(DeliveryMode::StartUp)
                         .with_level_assert()
                         .with_destination(dest_id);
    dispatch_icr(icr);
  }
};
} // namespace kernel::hw::apic