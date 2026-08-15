#include "utils/logger.hpp"
#include <uacpi/acpi.h>

namespace kernel::utils::logger {
drivers::uart::SerialPort &get_debug_console(const void *spcr_raw) noexcept {
  static std::optional<drivers::uart::SerialPort> s_console;
  static bool s_acpi_configured = false;

  // If SPCR is provided and we haven't upgraded yet, reconfigure the console.
  if (const auto *spcr = static_cast<const acpi_spcr *>(spcr_raw); spcr != nullptr && !s_acpi_configured) [[unlikely]] {
    const auto type = (spcr->base_address.address_space_id == 0) ? hw::IoType::MMIO : hw::IoType::PMIO;
    const std::uint8_t stride = (spcr->base_address.access_size > 0) ? spcr->base_address.access_size : 1;

    drivers::uart::SerialPort acpi_port(hw::IoResource(type, spcr->base_address.address_space_id, stride));

    if (acpi_port.initialize(115200).has_value()) {
      s_console.emplace(acpi_port);
      s_acpi_configured = true;

      s_console->append("[CONSOLE] Upgrade to ACPI SPCR routing.\n");
      return *s_console;
    }
  }

  // If it's the very first call, initialize the legacy fallback PMIO console.
  if (!s_console.has_value()) [[unlikely]] {
    drivers::uart::SerialPort early_port(hw::IoResource(hw::IoType::PMIO, 0x3f8, 1));

    early_port.initialize(115200);
    s_console.emplace(early_port);
  }

  return *s_console;
}
} // namespace kernel::utils::logger
