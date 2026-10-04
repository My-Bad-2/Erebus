#pragma once

#include "../io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/errors.hpp"
#include "memory/address.hpp"
#include "uacpi/acpi.h"
#include "utils/lock.hpp"

#include "apic.hpp"
#include "utils/vector.hpp"
#include <bitfield.hpp>

namespace kernel::hw::apic {
struct IoApicConfig {
  static constexpr std::size_t max_controllers = 8;
  static constexpr std::size_t max_isa_overrides = 16;
  static constexpr std::size_t max_pins_per_controller = 120;
};

enum IoApicOffsets : std::size_t {
  IOREGSEL = 0x00,
  IOWIN = 0x10,
  IOAPICEOI = 0x40,

  IdReg = 0x00,
  VersionReg = 0x01,

  RedirTableBase = 0x10,
  RedirEntryStride = 2,
};

struct IoRegSel {
private:
  std::uint32_t m_data{0};

public:
  constexpr IoRegSel(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RW(std::uint8_t, reg_index, 0, 8)
};

struct IoWin {
private:
  std::uint32_t m_data{0};

public:
  constexpr IoWin(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RW(std::uint32_t, data, 0, 32)
};

struct IoApicEoi {
private:
  std::uint32_t m_data{0};

public:
  constexpr IoApicEoi(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RW(std::uint8_t, vector, 0, 8)
};

struct IoApicId {
private:
  std::uint32_t m_data{0};

public:
  constexpr IoApicId(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RW(std::uint8_t, apic_id, 24, 8)
};

struct IoApicVer {
private:
  std::uint32_t m_data{0};

public:
  constexpr IoApicVer(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RO(std::uint8_t, version, 0, 8)
  BF_RO(std::uint8_t, max_redirection_entries, 16, 8)
};

struct RedirectionEntryLow {
private:
  std::uint32_t m_data{0};

public:
  constexpr RedirectionEntryLow(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, vector, 0, 8)
  BF_RW(DeliveryMode, delivery_mode, 8, 3)

  BF_RW(DestinationMode, destination_mode, 11, 1)
  BF_BIT_RW(interrupt_format, 11) // IOMMU VT-d Remapping mode toggle

  BF_BIT_RO(delivery_status, 12)
  BF_RW(PinPolarity, pin_polarity, 13, 1)
  BF_BIT_RO(remote_irr, 14)
  BF_RW(TriggerMode, trigger_mode, 15, 1)
  BF_BIT_RW(mask, 16)
};

struct RedirectionEntryHigh {
private:
  std::uint32_t m_data{0};

public:
  constexpr RedirectionEntryHigh(const std::uint32_t v = 0) : m_data(v) {}
  constexpr std::uint32_t raw() const noexcept { return m_data; }
  BF_RW(std::uint8_t, destination, 24, 8)
  BF_RW(std::uint16_t, interrupt_index, 2, 15) // Index into the IOMMU IRT
};

struct ShadowEntry {
private:
  RedirectionEntryLow m_low;
  RedirectionEntryHigh m_high;

public:
  constexpr ShadowEntry(const RedirectionEntryLow lo = {}, const RedirectionEntryHigh hi = {})
      : m_low(lo), m_high(hi) {}

  constexpr RedirectionEntryLow low() const noexcept { return m_low; }
  constexpr RedirectionEntryHigh high() const noexcept { return m_high; }

  constexpr void set_low(const std::uint32_t lo) noexcept { m_low = lo; }
  constexpr void set_high(const std::uint32_t hi) noexcept { m_high = hi; }

  [[nodiscard]] constexpr std::uint64_t pack() const noexcept {
    return (static_cast<std::uint64_t>(m_high.raw()) << 32) | m_low.raw();
  }

  [[nodiscard]] static constexpr ShadowEntry unpack(const std::uint64_t val) noexcept {
    return ShadowEntry(static_cast<std::uint32_t>(val), static_cast<std::uint32_t>(val >> 32));
  }
};

class IoApicController {
  MmioResource m_io{};
  std::uint32_t m_gsi_base{0};
  std::uint32_t m_max_entries{0};
  std::uint8_t m_version{0};

  std::atomic<std::uint64_t> *m_shadow_table{nullptr};
  mutable utils::QSpinlock m_lock;

  template <typename T> [[nodiscard]] T read_direct(const std::size_t offset) const noexcept {
    return T{m_io.read<std::uint32_t>(offset)};
  }

  template <typename T> void write_direct(const std::size_t offset, T val) const noexcept {
    m_io.write<std::uint32_t>(offset, val.raw());
  }

  template <typename Schema> [[nodiscard]] Schema read_indirect(const std::uint8_t reg) const noexcept {
    utils::IrqSaveGuard guard{m_lock};
    write_direct(IOREGSEL, IoRegSel{}.with_reg_index(reg));
    return Schema{read_direct<IoWin>(IOWIN).get_data()};
  }

  void write_indirect_unsafe(const std::uint8_t reg, const std::uint32_t data) const noexcept {
    write_direct(IOREGSEL, IoRegSel{}.with_reg_index(reg));
    write_direct(IOWIN, IoWin{}.with_data(data));
  }

public:
  constexpr IoApicController() noexcept = default;
  ~IoApicController() { delete[] m_shadow_table; }

  IoApicController(const IoApicController &) = delete;
  IoApicController &operator=(const IoApicController &) = delete;

  IoApicController(IoApicController &&other) noexcept
      : m_io(other.m_io), m_gsi_base(other.m_gsi_base), m_max_entries(other.m_max_entries), m_version(other.m_version),
        m_shadow_table(std::exchange(other.m_shadow_table, nullptr)), m_lock() {}

  IoApicController &operator=(IoApicController &&other) noexcept {
    if (this != &other) {
      delete[] m_shadow_table;
      m_io = other.m_io;
      m_gsi_base = other.m_gsi_base;
      m_max_entries = other.m_max_entries;
      m_version = other.m_version;
      m_shadow_table = std::exchange(other.m_shadow_table, nullptr);
    }

    return *this;
  }

  std::expected<void, Error> initialize(memory::VirtualAddress mmio_base, std::uint32_t gsi_base) noexcept;

  [[nodiscard]] constexpr std::uint32_t get_gsi_base() const noexcept { return m_gsi_base; }
  [[nodiscard]] constexpr std::uint32_t get_max_entries() const noexcept { return m_max_entries; }
  [[nodiscard]] constexpr bool handles_gsi(const std::uint32_t gsi) const noexcept {
    return gsi >= m_gsi_base && gsi < (m_gsi_base + m_max_entries);
  }

  constexpr std::uint8_t version() const noexcept { return m_version; }

  [[nodiscard]] std::uint64_t get_cached_state(const std::uint32_t gsi) const noexcept {
    const std::uint32_t pin = gsi - m_gsi_base;
    return m_shadow_table[pin].load(std::memory_order_acquire);
  }

  void configure_gsi(std::uint32_t gsi, RedirectionEntryLow low, RedirectionEntryHigh high) const noexcept;
  void set_mask(std::uint32_t gsi, bool mask) const noexcept;

  void send_eoi(const std::uint8_t vector) const noexcept {
    if (m_version >= 0x20) {
      write_direct(IOAPICEOI, IoApicEoi{}.with_vector(vector));
    }
  }
};

struct InterruptOverride {
  std::uint32_t gsi;
  std::uint16_t flags;
  bool active;
};

class IoApicManager {
  utils::Vector<IoApicController> m_controllers{};
  utils::Vector<InterruptOverride> m_overrides{};

  [[nodiscard]] uint32_t resolve_isa_to_gsi(std::uint8_t isa_irq) const noexcept;
  [[nodiscard]] std::pair<PinPolarity, TriggerMode> resolve_acpi_flags(std::uint8_t isa_irq) const noexcept;
  IoApicController *find_controller(std::uint32_t gsi) noexcept;

public:
  IoApicManager() = default;

  std::expected<void, Error> register_controller(memory::VirtualAddress mmio_base, std::uint32_t gsi_base) noexcept;
  std::expected<void, Error> register_isa_override(std::uint8_t isa_irq, std::uint32_t mapped_gsi,
                                                   std::uint16_t acpi_flags) noexcept;

  std::expected<void, Error> route_isa_irq(std::uint8_t isa_irq, std::uint8_t vector, std::uint8_t apic_id,
                                           bool masked = false) noexcept;
  std::expected<void, Error> route_direct_gsi(std::uint32_t gsi, std::uint8_t vector, std::uint8_t apic_id,
                                              PinPolarity polarity = PinPolarity::High,
                                              TriggerMode trigger = TriggerMode::Edge, bool masked = false) noexcept;

  std::expected<void, Error> route_remapped_gsi(std::uint32_t gsi, std::uint16_t iommu_irt_index,
                                                bool masked = true) noexcept;
  std::expected<void, Error> mask_gsi(std::uint32_t gsi) noexcept;
  std::expected<void, Error> unmask_gsi(std::uint32_t gsi) noexcept;

  bool supports_directed_eoi() const noexcept { return m_controllers[0].version() >= 0x20; }
};

extern IoApicManager io_apic_manager;
} // namespace kernel::hw::apic