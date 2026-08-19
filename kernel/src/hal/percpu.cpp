#include "hal/percpu.hpp"
#include "hal/hal.hpp"

namespace kernel::hw::percpu {
namespace {
PerCpu bsp;
}

void early_initialize() noexcept {
  new (&bsp) PerCpu();
  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));
}
} // namespace kernel::hw::percpu