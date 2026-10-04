#include "hal/apic/ioapic.hpp"

#include "hal/interrupt_manager.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::apic {
IoApicManager io_apic_manager;

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
  if (ver.raw() == 0xFFFFFFFF) {
    return std::unexpected(Error::HardwareError);
  }

  m_version = ver.get_version();
  m_max_entries = ver.get_max_redirection_entries() + 1;

  m_shadow_table = new std::atomic<std::uint64_t>[m_max_entries] {};
  if (!m_shadow_table) {
    return std::unexpected(Error::OutOfMemory);
  }

  for (std::uint32_t pin = 0; pin < m_max_entries; ++pin) {
    constexpr auto low = RedirectionEntryLow{}.with_mask(true);
    configure_gsi(m_gsi_base + pin, low, RedirectionEntryHigh{});
  }

  return {};
}

[[nodiscard]] std::pair<PinPolarity, TriggerMode>
IoApicManager::resolve_acpi_flags(const std::uint8_t isa_irq) const noexcept {
  auto polarity = PinPolarity::High;
  auto trigger = TriggerMode::Edge;

  const auto *override = m_overrides.at(isa_irq);
  if (override && override->active) {
    if ((override->flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW) {
      polarity = PinPolarity::Low;
    }

    if ((override->flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL) {
      trigger = TriggerMode::Level;
    }
  }

  return {polarity, trigger};
}

[[nodiscard]] uint32_t IoApicManager::resolve_isa_to_gsi(const std::uint8_t isa_irq) const noexcept {
  auto *override = m_overrides.at(isa_irq);
  if (override && override->active) {
    return override->gsi;
  }

  return isa_irq; // 1-to-1 mapping
}

IoApicController *IoApicManager::find_controller(const std::uint32_t gsi) noexcept {
  for (auto &controller : m_controllers) {
    if (controller.handles_gsi(gsi)) {
      return &controller;
    }
  }

  return nullptr;
}

std::expected<void, Error> IoApicManager::register_controller(const memory::VirtualAddress mmio_base,
                                                              const std::uint32_t gsi_base) noexcept {
  IoApicController controller;
  if (auto res = controller.initialize(mmio_base, gsi_base); !res) {
    return res;
  }

  if (const auto res = m_controllers.push_back(std::move(controller)); !res) {
    return std::unexpected(Error::OutOfMemory);
  }

  return {};
}

std::expected<void, Error> IoApicManager::register_isa_override(std::uint8_t isa_irq, std::uint32_t mapped_gsi,
                                                                std::uint16_t acpi_flags) noexcept {
  if (isa_irq >= m_overrides.size()) {
    if (auto res = m_overrides.resize(isa_irq + 1); !res) {
      return std::unexpected(Error::OutOfMemory);
    }
  }

  if (auto *override = m_overrides.at(isa_irq)) {
    *override = {
        .gsi = mapped_gsi,
        .flags = acpi_flags,
        .active = true,
    };
  }

  return {};
}

std::expected<void, Error> IoApicManager::route_isa_irq(std::uint8_t isa_irq, std::uint8_t vector, std::uint8_t apic_id,
                                                        bool masked) noexcept {
  const std::uint32_t gsi = resolve_isa_to_gsi(isa_irq);
  auto [polarity, trigger] = resolve_acpi_flags(isa_irq);
  return route_direct_gsi(gsi, vector, apic_id, polarity, trigger, masked);
}

std::expected<void, Error> IoApicManager::route_direct_gsi(const std::uint32_t gsi, const std::uint8_t vector,
                                                           const std::uint8_t apic_id, const PinPolarity polarity,
                                                           const TriggerMode trigger, const bool masked) noexcept {
  IoApicController *controller = find_controller(gsi);
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
  interrupts::InterruptManager::set_eoi_strategy(vector, (trigger == TriggerMode::Level), controller);
  return {};
}

std::expected<void, Error> IoApicManager::route_remapped_gsi(std::uint32_t gsi, std::uint16_t iommu_irt_index,
                                                             bool masked) noexcept {
  IoApicController *controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  constexpr auto entry_low = RedirectionEntryLow{}.with_interrupt_format().with_mask();
  controller->configure_gsi(gsi, entry_low, RedirectionEntryHigh{}.with_interrupt_index(iommu_irt_index));
  return {};
}

std::expected<void, Error> IoApicManager::mask_gsi(std::uint32_t gsi) noexcept {
  IoApicController *controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  controller->set_mask(gsi, true);
  return {};
}

std::expected<void, Error> IoApicManager::unmask_gsi(std::uint32_t gsi) noexcept {
  IoApicController *controller = find_controller(gsi);
  if (!controller) {
    return std::unexpected(Error::UnmappedGSI);
  }

  controller->set_mask(gsi, false);
  return {};
}
} // namespace kernel::hw::apic