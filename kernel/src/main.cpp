#include "drivers/acpi.hpp"
#include "drivers/uart.hpp"

#include "utils/hashmap.hpp"
#include "utils/logger.hpp"

#include "hal/cpu_info.hpp"
#include "hal/hal.hpp"
#include "hal/patcher.hpp"
#include "hal/percpu.hpp"
#include "memory/memory.hpp"

namespace kernel {
namespace {
std::uint32_t runqueue_count = 0;
}

extern "C" void _start() noexcept {
  drivers::uart::SerialPort &sink = utils::logger::get_debug_console();
  sink.append("\x1b[2J"); // Clear screen on host (can be safely removed)

  utils::logger::info("Hello, World!\n");

  hw::percpu::early_initialize();
  hw::initialize();
  hw::patcher::apply_all_boot_patches();

  drivers::acpi::early_initialize();
  memory::initialize();

  utils::logger::info("Hello, World!\n");
  hw::cpu_idle_loop(&runqueue_count);
}
} // namespace kernel
