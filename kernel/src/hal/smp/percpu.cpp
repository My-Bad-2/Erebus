#include "hal/smp/percpu.hpp"

#include "hal/hal.hpp"
#include "hal/smp/smp.hpp"

namespace kernel::hw::percpu {
namespace {
constinit PerCpu bsp{};
}  // namespace

PerCpu* cpu_area(const std::uint32_t cpu_id) noexcept {
  if (smp::detail::g_cpus) [[likely]] {
    return smp::entry(cpu_id).area;
  }

  return cpu_id == 0 ? &bsp : nullptr;
}

void install(PerCpu* cpu) noexcept {
  cpu->gdt_table.load();
  write::gs_base(reinterpret_cast<std::uintptr_t>(cpu));
}

void early_initialize() noexcept {
  install(&bsp);

  bsp.info = profile_manager.register_cpu();
  memory::vmm::early_initialize_hw();
}
}  // namespace kernel::hw::percpu
