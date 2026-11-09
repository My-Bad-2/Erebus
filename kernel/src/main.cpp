#include "boot/boot.hpp"
#include "crypto/random.hpp"
#include "drivers/acpi.hpp"
#include "drivers/clock.hpp"
#include "drivers/uart.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/memory.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "patch/static_call.hpp"
#include "proc/process.hpp"
#include "sched/reaper.hpp"
#include "sched/scheduler.hpp"
#include "sched/workqueue.hpp"
#include "utils/logger.hpp"

#ifdef EREBUS_KERNEL_TESTS
#include "ktest.hpp"
#endif

namespace kernel {
[[noreturn]] void kmain(void* stack) {
  const memory::VirtualAddress bsp_stack{stack};

#ifdef EREBUS_KERNEL_TESTS
  // The test kernel keeps the APs for its multi-CPU tests.
  hw::smp::set_ap_main(&testing::run_on_ap);
#endif

  hw::smp::initialize(bsp_stack);

  // The kernel's own threads, now that every CPU can run them.
  sched::reaper::initialize();
  sched::workqueue::initialize();
  memory::pmm::initialize_daemons();
  proc::initialize();

  utils::logger::info("Hello, World!\n");

#ifdef EREBUS_KERNEL_TESTS
  testing::run_all();
#endif

  sched::Scheduler::enter_idle_loop();
}

extern "C" void _start() noexcept {
  utils::logger::info("Hello, World!\n");
  boot::validate();

  hw::percpu::early_initialize();
  hw::initialize();
  crypto::random::initialize();

  drivers::acpi::early_initialize();
  memory::initialize();

  drivers::SystemClock::initialize();

  if (auto res = patch::StaticCallPatcher::apply_patches(); !res) {
    utils::logger::fatal("Failed to apply static call patches: error {:#x}\n", res.error());
  }
  const auto bsp_rsp = memory::vmm::kernel_space()->alloc(
      KSTACK_SIZE, memory::vmm::AccessFlags::Read | memory::vmm::AccessFlags::Write |
                       memory::vmm::AccessFlags::Stack | memory::vmm::AccessFlags::Populate);
  if (!bsp_rsp) {
    utils::logger::fatal("Failed to allocate BSP stack! error : {}\n",
                         std::to_underlying(bsp_rsp.error()));
  }

  const memory::VirtualAddress stack = *bsp_rsp + KSTACK_SIZE;
  hw::execute_on_new_stack(stack.value(), stack.as<void>(), kmain);
  // WARNING: Don't add anything here. The kernel has jumped to kmain() and will
  // continue executing from there.
}
}  // namespace kernel
