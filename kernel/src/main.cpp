#include "drivers/acpi.hpp"
#include "drivers/uart.hpp"
#include "utils/logger.hpp"

#include "hal/cpu_info.hpp"
#include "hal/patcher.hpp"
#include "memory/address.hpp"

namespace kernel {
extern "C" void _start() noexcept {
  drivers::uart::SerialPort &sink = utils::logger::get_debug_console();
  sink.append("\x1b[2J"); // Clear screen on host (can be safely removed)

  utils::logger::info("Hello, World!\n");

  auto cpu_info = hw::profile_manager.register_cpu();
  hw::patcher::apply_all_boot_patches(0);

  drivers::acpi::early_initialize();

  memory::PhysicalAddress addr{0x1020};

  utils::logger::info("{} by {}\n", cpu_info->brand_string(), cpu_info->vendor_string());
  utils::logger::info("{}\n", addr);

  utils::logger::info("Hello, World!\n");
  while (true) {
  }
}
} // namespace kernel
