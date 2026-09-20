#include "hal/percpu.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::hw::percpu {
namespace {
PerCpu bsp;
constexpr std::size_t CRITICAL_STACK_SIZE = 2 * memory::PAGE_SIZE;
} // namespace

void early_initialize() noexcept {
  new (&bsp) PerCpu();
  bsp.gdt_table.load();

  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));

  [[maybe_unused]] auto _ = profile_manager.register_cpu();

  memory::vmm::early_initialize_hw();
}

void initialize_interrupts(memory::VirtualAddress rsp) noexcept {
  using namespace memory::vmm;
  auto kspace = kernel_space();

  const auto critical_stack_res =
      kspace->alloc(CRITICAL_STACK_SIZE, AccessFlags::Read | AccessFlags::Write | AccessFlags::Stack);
  if (!critical_stack_res) {
    utils::logger::fatal("Failed to allocate critical stack!\n");
  }

  bsp.gdt_table.set_interrupt_stack(gdt::IstIndex::Ist1, *critical_stack_res);
  bsp.gdt_table.set_kernel_stack(rsp);
  interrupts::initialize(rsp, *critical_stack_res);
}
} // namespace kernel::hw::percpu