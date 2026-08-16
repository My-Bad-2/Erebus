#include "drivers/acpi.hpp"
#include "drivers/uart.hpp"

#include "utils/logger.hpp"

#include "hal/cpu_info.hpp"
#include "hal/hal.hpp"
#include "hal/patcher.hpp"

namespace kernel {
namespace {
std::uint32_t runqueue_count = 0;
}

extern "C" void _start() noexcept {
  drivers::uart::SerialPort &sink = utils::logger::get_debug_console();
  sink.append("\x1b[2J"); // Clear screen on host (can be safely removed)

  utils::logger::info("Hello, World!\n");

  auto cpu_info = hw::profile_manager.register_cpu();
  hw::initialize();
  hw::patcher::apply_all_boot_patches();

  drivers::acpi::early_initialize();

  utils::logger::info("{} by {}\n", cpu_info->brand_string(), cpu_info->vendor_string());

  utils::logger::info("Hello, World!\n");
  hw::cpu_idle_loop(&runqueue_count);
}
} // namespace kernel
