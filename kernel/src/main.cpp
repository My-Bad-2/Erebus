#include "drivers/acpi.hpp"
#include "drivers/uart.hpp"

#include "utils/hashmap.hpp"
#include "utils/logger.hpp"

#include "hal/hal.hpp"
#include "hal/patcher.hpp"
#include "hal/percpu.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel {
namespace {
std::uint32_t runqueue_count = 0;
} // namespace

[[noreturn]] void kmain(void *stack) {
  const memory::VirtualAddress bsp_stack{stack};
  hw::percpu::initialize(bsp_stack);

  utils::logger::info("Hello, World!\n");
  hw::cpu_idle_loop(&runqueue_count);
}

extern "C" void _start() noexcept {
  const drivers::uart::SerialPort &sink = utils::logger::get_debug_console();
  sink.append("\x1b[2J"); // Clear screen on host (can be safely removed)

  utils::logger::info("Hello, World!\n");

  hw::percpu::early_initialize();
  hw::initialize();
  hw::patcher::apply_all_boot_patches();

  drivers::acpi::early_initialize();
  memory::initialize();

  const auto bsp_rsp = memory::vmm::kernel_space()->alloc(
      KSTACK_SIZE, memory::vmm::AccessFlags::Read | memory::vmm::AccessFlags::Write | memory::vmm::AccessFlags::Stack |
                       memory::vmm::AccessFlags::Populate);
  if (!bsp_rsp) {
    utils::logger::fatal("Failed to allocate BSP stack! error : {}\n", std::to_underlying(bsp_rsp.error()));
  }

  const memory::VirtualAddress stack = *bsp_rsp + KSTACK_SIZE;
  hw::execute_on_new_stack(stack.value(), stack.as<void>(), kmain);
  // WARNING: Don't add anything here. The kernel has jumped to kmain() and will continue executing from there.
}
} // namespace kernel
