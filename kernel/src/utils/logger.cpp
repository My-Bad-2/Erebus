#include "utils/logger.hpp"

#include <uacpi/acpi.h>

namespace kernel::utils::logger {
namespace {
std::optional<drivers::uart::SerialPort> console;
TicketSpinlock console_lock;

void ensure_console_init() noexcept {
  if (!console.has_value()) [[unlikely]] {
    drivers::uart::SerialPort early_port(
        hw::IoResource(hw::IoType::PMIO, 0x3f8, 1));
    auto _ = early_port.initialize(115200);
    console.emplace(early_port);
    console->append("\x1b[2J");  // Clear screen on host (can be safely removed)
  }
}
}  // namespace

void initialize(const void* spcr_raw) noexcept {
  if (!spcr_raw) return;

  NakedGuard guard(console_lock);

  const auto* spcr = static_cast<const acpi_spcr*>(spcr_raw);
  if (spcr != nullptr) [[unlikely]] {
    const auto type = spcr->base_address.address_space_id == 0
                          ? hw::IoType::MMIO
                          : hw::IoType::PMIO;
    const std::uint8_t stride =
        spcr->base_address.access_size > 0 ? spcr->base_address.access_size : 1;

    drivers::uart::SerialPort acpi_port(
        hw::IoResource(type, spcr->base_address.address_space_id, stride));

    if (acpi_port.initialize(115200).has_value()) {
      console.emplace(acpi_port);

      auto msg = "[CONSOLE] Upgrade to ACPI SPCR routing.\n";
      while (*msg) console->push(*msg++);
    }
  }
}

void write_console(const std::string_view msg) noexcept {
  NakedGuard guard(console_lock);
  ensure_console_init();

  for (const char c : msg) console->push(c);
}
}  // namespace kernel::utils::logger
