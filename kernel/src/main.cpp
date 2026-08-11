#include "drivers/acpi.hpp"
#include "drivers/uart.hpp"
#include "utils/logger.hpp"
#include <kformat.hpp>
#include <stddef.h>

#include "memory/address.hpp"
#include "utils/maths.hpp"

namespace kernel {
extern "C" void _start() noexcept {
  drivers::uart::SerialPort &sink = utils::logger::get_debug_console();
  sink.append("\x1b[2J"); // Clear screen on host (can be safely removed)

  drivers::acpi::early_initialize();

  memory::PhysicalAddress addr{0x1020};

  utils::logger::info("{}\n", addr);

  utils::logger::info("Hello, World!\n");
  while (true) {
  }
}
} // namespace kernel
