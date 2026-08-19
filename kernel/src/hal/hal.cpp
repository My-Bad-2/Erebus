#include "hal/hal.hpp"
#include "hal/cpu_info.hpp"

namespace kernel::hw {
void initialize() noexcept {
  const CpuInfo *cpu = profile_manager.get_current();
  detail::use_mwait.enabled = cpu->has<Feature::MONITOR>() && !cpu->has<Feature::HYPERVISOR>();
  detail::use_waitpkg.enabled = cpu->has<Feature::WAITPKG>();
  detail::use_moniterx.enabled = cpu->has<Feature::MONITORX>();
  detail::use_fsgsbase.enabled = cpu->has<Feature::FSGSBASE>();

  if (detail::use_fsgsbase.enabled) {
    CR4Schema cr4 = read::cr4().set<"fsgsbase">(1);
    write::cr4(cr4);
  }
}
} // namespace kernel::hw