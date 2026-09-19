#include "hal/percpu.hpp"
#include "hal/hal.hpp"

namespace kernel::hw::percpu {
namespace {
PerCpu bsp;
}

void early_initialize() noexcept {
  new (&bsp) PerCpu();
  bsp.gdt_table.load();
  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));

  [[maybe_unused]] auto _ = profile_manager.register_cpu();
  memory::vmm::early_initialize_hw();
}
} // namespace kernel::hw::percpu