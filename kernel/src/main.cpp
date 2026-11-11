#include <expected>
#include <span>

#include "boot/boot.hpp"
#include "core/cap.hpp"
#include "core/cspace.hpp"
#include "core/ipc/object_cap.hpp"
#include "crypto/random.hpp"
#include "drivers/acpi.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/memory.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "patch/static_call.hpp"
#include "proc/elf.hpp"
#include "proc/process.hpp"
#include "sched/reaper.hpp"
#include "sched/scheduler.hpp"
#include "sched/workqueue.hpp"
#include "utils/logger.hpp"

#ifdef EREBUS_KERNEL_TESTS
#include "ktest.hpp"
#endif

namespace kernel {
namespace {
std::expected<void, Error> give_ipc_context(sched::Thread& thread, void*) noexcept {
  auto cspace = core::ipc::CSpace::create_charged(thread.process()->quota());
  if (!cspace) [[unlikely]] {
    return std::unexpected(cspace.error());
  }

  return core::ipc::ThreadCap::attach(thread, cspace->get()).transform([](const auto&) {});
}

void start_root_task() noexcept {
  const limine_module_response* modules = boot::module_request.response;
  if (modules == nullptr || modules->module_count == 0) {
    utils::logger::warn("init: no root task among the boot modules, so nothing runs in ring 3\n");
    return;
  }

  const limine_file& file = *modules->modules[0];
  const std::span image{static_cast<const std::byte*>(file.address), file.size};

  const sched::UserThreadCreateOptions opts{
      .priority = sched::PriorityClass::Normal,
      .preferred_cpu = hw::percpu::id(),
      .pinned = true,
  };

  const auto spawned =
      proc::elf::spawn_from_elf("root_task", image, 0, opts, nullptr, &give_ipc_context, nullptr);
  if (!spawned) {
    utils::logger::fatal("init: cannot start the root task from {}: {:#x}\n", file.path,
                         std::to_underlying(spawned.error()));
  }

  utils::logger::info("init: started {} (pid {}, tid {}) from {} at {:#x}\n",
                      spawned->process->name(), spawned->process->pid(), spawned->tid, file.path,
                      spawned->image.entry.value());
}
}  // namespace

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
  core::ipc::initialize();
  memory::pmm::initialize_daemons();
  proc::initialize();

  utils::logger::info("Hello, World!\n");

#ifdef EREBUS_KERNEL_TESTS
  testing::run_all();
#endif

  start_root_task();

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
