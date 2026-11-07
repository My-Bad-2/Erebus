#include "drivers/acpi.hpp"

#include <uacpi/uacpi.h>

#include <cstddef>

#include "drivers/uart.hpp"
#include "utils/logger.hpp"

namespace kernel::drivers::acpi {
namespace {
alignas(16) std::byte early_buf[8192];

[[nodiscard]] constexpr bool is_16550_compatible(const std::uint8_t interface_type) noexcept {
  return interface_type == 0x00 || interface_type == 0x01 || interface_type == 0x12;
}

[[nodiscard]] constexpr std::uint32_t decode_baud(const std::uint8_t code) noexcept {
  switch (code) {
    case 3:
      return 9600;
    case 4:
      return 19200;
    case 6:
      return 57600;
    case 7:
      return 115200;
    default:
      return 0;
  }
}

void setup_spcr() noexcept {
  auto spcr_res = Table::find<"SPCR">();
  if (!spcr_res) [[unlikely]] {
    utils::logger::info("No SPCR table, keeping the console on COM1.\n");
    return;
  }

  const Table spcr_table = std::move(*spcr_res);
  const auto spcr_ptr = spcr_table.as<acpi_spcr>(offsetof(acpi_spcr, uart_clock_frequency));
  if (!spcr_ptr) [[unlikely]] {
    utils::logger::error("Malformed SPCR table.\n");
    return;
  }

  const acpi_spcr* spcr = *spcr_ptr;
  if (!is_16550_compatible(spcr->interface_type)) {
    utils::logger::info("SPCR console is interface type {:#x}, not a 16550; ignoring it.\n",
                        spcr->interface_type);
    return;
  }

  const acpi_gas& gas = spcr->base_address;
  if (gas.address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_IO || gas.address == 0 ||
      gas.address > 0xFFFF) {
    utils::logger::warn("SPCR console at {:#x} (address space {}) is not an I/O port; ignoring.\n",
                        gas.address, gas.address_space_id);
    return;
  }

  std::uint32_t baud = decode_baud(spcr->configured_baud_rate);
  std::uint32_t clock_hz = 0;

  if (spcr_table.revision() >= 3 &&
      spcr_table.length() >= offsetof(acpi_spcr, uart_clock_frequency) + sizeof(std::uint32_t)) {
    clock_hz = spcr->uart_clock_frequency;
  }

  if (spcr_table.revision() >= 4 &&
      spcr_table.length() >= offsetof(acpi_spcr, precise_baud_rate) + sizeof(std::uint32_t) &&
      spcr->precise_baud_rate != 0) {
    baud = spcr->precise_baud_rate;
  }

  uart::SerialPort port{hw::IoResource{hw::IoType::PMIO, gas.address, 1}};
  if (const auto res = port.initialize(baud, clock_hz); !res) {
    utils::logger::warn("SPCR console at {:#x} is unusable: {}\n", gas.address, res.error());
    return;
  }

  utils::logger::initialize(port);
}
}  // namespace

void early_initialize() noexcept {
  if (const auto st = uacpi_setup_early_table_access(early_buf, sizeof(early_buf));
      st != UACPI_STATUS_OK) [[unlikely]] {
    utils::logger::error("ACPI tables unavailable: {}\n", uacpi_status_to_string(st));
    return;
  }

  setup_spcr();
}
}  // namespace kernel::drivers::acpi
