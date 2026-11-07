#include "hal/apic/ioapic.hpp"

#include <memory>

#include "drivers/acpi.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "memory/heap.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::apic {
namespace {
void disable_legacy_pic() noexcept {
  constexpr IoResource master{IoType::PMIO, 0x20};
  constexpr IoResource slave{IoType::PMIO, 0xA0};
  constexpr IoResource delay{IoType::PMIO, 0x80};

  constexpr std::uint8_t ICW1_INIT_WITH_ICW4 = 0x11;
  constexpr std::uint8_t ICW4_8086 = 0x01;

  const auto out = [&](const IoResource& port, const std::size_t reg, const std::uint8_t val) {
    port.write<std::uint8_t>(reg, val);
    delay.write<std::uint8_t>(0, 0);
  };

  out(master, 0, ICW1_INIT_WITH_ICW4);
  out(slave, 0, ICW1_INIT_WITH_ICW4);
  out(master, 1, 0x20);  // ICW2: vectors 0x20-0x27
  out(slave, 1, 0x28);   // ICW2: vectors 0x28-0x2F
  out(master, 1, 0x04);  // ICW3: slave on IRQ 2
  out(slave, 1, 0x02);   // ICW3: cascade identity
  out(master, 1, ICW4_8086);
  out(slave, 1, ICW4_8086);

  out(master, 1, 0xFF);
  out(slave, 1, 0xFF);
}
}  // namespace

[[clang::no_destroy]] constinit IoApicManager io_apic_manager;

IoApicController::~IoApicController() { memory::heap::kfree(m_shadow_table); }

IoApicController& IoApicController::operator=(IoApicController&& other) noexcept {
  if (this != &other) {
    memory::heap::kfree(m_shadow_table);
    m_io = other.m_io;
    m_gsi_base = other.m_gsi_base;
    m_max_entries = other.m_max_entries;
    m_version = other.m_version;
    m_shadow_table = std::exchange(other.m_shadow_table, nullptr);
  }

  return *this;
}

void IoApicController::configure_gsi(const std::uint32_t gsi, const RedirectionEntryLow low,
                                     const RedirectionEntryHigh high) const noexcept {
  const std::uint32_t pin = gsi - m_gsi_base;
  const ShadowEntry entry{low.raw(), high.raw()};

  utils::IrqSaveGuard guard{m_lock};
  m_shadow_table[pin].store(entry.pack(), std::memory_order_release);

  const std::uint8_t offset = RedirTableBase + (pin * RedirEntryStride);
  write_indirect_unsafe(offset + 1, entry.high().raw());
  write_indirect_unsafe(offset, entry.low().raw());
}

void IoApicController::set_mask(const std::uint32_t gsi, const bool mask) const noexcept {
  const std::uint32_t pin = gsi - m_gsi_base;
  utils::IrqSaveGuard guard(m_lock);

  auto entry = ShadowEntry::unpack(m_shadow_table[pin].load(std::memory_order_relaxed));
  RedirectionEntryLow low{entry.low()};

  if (low.get_mask() == mask) {
    return;
  }

  low.set_mask(mask);
  entry.set_low(low.raw());
  m_shadow_table[pin].store(entry.pack(), std::memory_order_release);

  const std::uint8_t reg_offset = RedirTableBase + (pin * RedirEntryStride);
  write_indirect_unsafe(reg_offset, entry.low().raw());
}

std::expected<void, Error> IoApicController::initialize(const memory::VirtualAddress mmio_base,
                                                        const std::uint32_t gsi_base) noexcept {
  m_io.set_base(mmio_base);
  m_gsi_base = gsi_base;

  const auto ver = read_indirect<IoApicVer>(VersionReg);
  if (ver.raw() == std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(Error::HardwareError);
  }

  m_version = ver.get_version();
  m_max_entries = ver.get_max_redirection_entries() + 1u;

  if (m_max_entries > IoApicConfig::max_pins_per_controller) [[unlikely]] {
    utils::logger::warn("IOAPIC at GSI {} claims {} pins, using the first {}\n", gsi_base,
                        m_max_entries, IoApicConfig::max_pins_per_controller);
    m_max_entries = IoApicConfig::max_pins_per_controller;
  }

  m_shadow_table = static_cast<std::atomic<std::uint64_t>*>(
      memory::heap::kmalloc(sizeof(std::atomic<std::uint64_t>) * m_max_entries));

  if (!m_shadow_table) {
    return std::unexpected(Error::OutOfMemory);
  }

  std::uninitialized_default_construct_n(m_shadow_table, m_max_entries);

  for (std::uint32_t pin = 0; pin < m_max_entries; ++pin) {
    constexpr auto low = RedirectionEntryLow{}.with_mask(true);
    configure_gsi(m_gsi_base + pin, low, RedirectionEntryHigh{});
  }

  return {};
}

[[nodiscard]] std::pair<PinPolarity, TriggerMode> IoApicManager::resolve_acpi_flags(
    const std::uint8_t isa_irq) const noexcept {
  auto polarity = PinPolarity::High;
  auto trigger = TriggerMode::Edge;

  if (isa_irq < m_overrides.size() && m_overrides[isa_irq].active) {
    const std::uint16_t flags = m_overrides[isa_irq].flags;

    if ((flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW) {
      polarity = PinPolarity::Low;
    }

    if ((flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL) {
      trigger = TriggerMode::Level;
    }
  }

  return {polarity, trigger};
}

[[nodiscard]] uint32_t IoApicManager::resolve_isa_to_gsi(std::uint8_t isa_irq) const noexcept {
  if (isa_irq < m_overrides.size() && m_overrides[isa_irq].active) {
    return m_overrides[isa_irq].gsi;
  }

  return isa_irq;  // 1-to-1 mapping
}

IoApicController* IoApicManager::find_controller(const std::uint32_t gsi) noexcept {
  for (auto& controller : m_controllers) {
    if (controller.handles_gsi(gsi)) {
      return &controller;
    }
  }

  return nullptr;
}

std::expected<void, Error> IoApicManager::register_controller(memory::VirtualAddress mmio_base,
                                                              std::uint32_t gsi_base) noexcept {
  IoApicController controller;
  auto controller_res = controller.initialize(mmio_base, gsi_base);
  if (!controller_res) {
    return controller_res;
  }

  const auto res = m_controllers.push_back(std::move(controller));
  if (!res) {
    return std::unexpected(Error::OutOfMemory);
  }

  return {};
}

std::expected<void, Error> IoApicManager::register_isa_override(std::uint8_t isa_irq,
                                                                std::uint32_t mapped_gsi,
                                                                std::uint16_t acpi_flags) noexcept {
  // ISA has 16 IRQs. Anything else in the MADT is firmware nonsense.
  if (isa_irq >= m_overrides.size()) {
    return std::unexpected(Error::OverrideLimitReached);
  }

  m_overrides[isa_irq] = {
      .gsi = mapped_gsi,
      .flags = acpi_flags,
      .active = true,
  };

  return {};
}

std::expected<void, Error> IoApicManager::route_isa_irq(std::uint8_t isa_irq, std::uint8_t vector,
                                                        std::uint8_t apic_id,
                                                        bool masked) noexcept {
  const std::uint32_t gsi = resolve_isa_to_gsi(isa_irq);
  auto [polarity, trigger] = resolve_acpi_flags(isa_irq);
  return route_direct_gsi(gsi, vector, apic_id, polarity, trigger, masked);
}

std::expected<void, Error> IoApicManager::route_direct_gsi(
    const std::uint32_t gsi, const std::uint8_t vector, const std::uint8_t apic_id,
    const PinPolarity polarity, const TriggerMode trigger, const bool masked) noexcept {
  IoApicController* controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  const auto low = RedirectionEntryLow{}
                       .with_vector(vector)
                       .with_delivery_mode(DeliveryMode::Fixed)
                       .with_destination_mode(DestinationMode::Physical)
                       .with_pin_polarity(polarity)
                       .with_trigger_mode(trigger)
                       .with_mask(masked);
  const auto high = RedirectionEntryHigh{}.with_destination(apic_id);

  controller->configure_gsi(gsi, low, high);
  interrupts::InterruptManager::set_eoi_strategy(vector, (trigger == TriggerMode::Level),
                                                 controller);
  return {};
}

std::expected<void, Error> IoApicManager::route_remapped_gsi(std::uint32_t gsi,
                                                             std::uint16_t iommu_irt_index,
                                                             bool masked) noexcept {
  const IoApicController* controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  const auto entry_low =
      RedirectionEntryLow{}.with_interrupt_index_15((iommu_irt_index >> 15) & 1).with_mask(masked);
  const auto entry_high = RedirectionEntryHigh{}.with_remappable_format().with_interrupt_index(
      iommu_irt_index & 0x7FFF);
  controller->configure_gsi(gsi, entry_low, entry_high);
  return {};
}

std::expected<void, Error> IoApicManager::mask_gsi(std::uint32_t gsi) noexcept {
  const IoApicController* controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  controller->set_mask(gsi, true);
  return {};
}

std::expected<void, Error> IoApicManager::unmask_gsi(std::uint32_t gsi) noexcept {
  const IoApicController* controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  controller->set_mask(gsi, false);
  return {};
}
void IoApicManager::initialize() noexcept {
  using namespace drivers::acpi;

  disable_legacy_pic();

  auto madt_res = Table::find<ACPI_MADT_SIGNATURE>();
  if (!madt_res) {
    utils::logger::fatal("MADT missing: {}\n", madt_res.error());
  }

  const Table madt = std::move(*madt_res);
  const auto payload_res = madt.payload<acpi_madt>();
  if (!payload_res) {
    utils::logger::fatal("MADT invalid: {}\n", payload_res.error());
  }

  // The register window is two 32-bit registers, IOREGSEL and IOWIN, 16 bytes apart. The EOI
  // register of newer IOAPICs sits at 0x40.
  constexpr std::size_t IOAPIC_REGISTER_BYTES = 0x44;

  for (const auto* hdr : *payload_res) {
    if (hdr->type == ACPI_MADT_ENTRY_TYPE_IOAPIC) {
      const auto* ioapic = entry_as<acpi_madt_ioapic>(hdr);
      if (!ioapic) {
        continue;
      }

      const auto mapped_base = memory::vmm::map_device_registers(
          memory::PhysicalAddress{ioapic->address}, IOAPIC_REGISTER_BYTES);
      if (!mapped_base) {
        utils::logger::fatal("Failed to map the IOAPIC at {:#x}\n", ioapic->address);
      }

      if (const auto res = register_controller(*mapped_base, ioapic->gsi_base); !res) {
        utils::logger::fatal("Failed to register the IOAPIC at {:#x}: {}\n", ioapic->address,
                             std::to_underlying(res.error()));
      }
    } else if (hdr->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE) {
      const auto* iso = entry_as<acpi_madt_interrupt_source_override>(hdr);

      // Only bus 0 (ISA) is defined.
      if (iso && iso->bus == 0 && !register_isa_override(iso->source, iso->gsi, iso->flags)) {
        utils::logger::warn("Ignoring MADT override for ISA IRQ {}\n", iso->source);
      }
    }
  }

  utils::logger::info("IOAPIC: {} controller(s), directed EOI {}\n", m_controllers.size(),
                      supports_directed_eoi() ? "on" : "off");
}
}  // namespace kernel::hw::apic
