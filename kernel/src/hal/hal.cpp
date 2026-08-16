#include "hal/hal.hpp"
#include "hal/cpu_info.hpp"

namespace kernel::hw {
void initialize() noexcept {
  const CpuInfo *cpu = profile_manager.get_current();
  detail::use_mwait = {.enabled = cpu->has<Feature::MONITOR>() && !cpu->has<Feature::HYPERVISOR>()};
  detail::use_tpause = {.enabled = cpu->has<Feature::WAITPKG>()};
}
} // namespace kernel::hw