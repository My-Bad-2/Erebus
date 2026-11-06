#include "hal/hal.hpp"

#include "hal/smp/cpu_info.hpp"
#include "hal/smp/percpu.hpp"
#include "ktest.hpp"

namespace kernel::testing {
namespace {
using hw::CpuInfo;
using hw::Feature;

static_assert(hw::map_leaf_to_index(0xD, 1) != 0xFFFFFFFF, "CPUID traps in a VM: keep this cached");

[[nodiscard]] constexpr bool bit(const std::uint32_t value, const std::uint32_t index) noexcept {
  return ((value >> index) & 1) != 0;
}
}  // namespace

// Both of these reach the instructions or the MSRs, whichever the CPU has, through a static call.
KTEST_ALL_CPUS(Hal, gs_base_points_at_the_per_cpu_area, cpu) {
  KEXPECT_EQ(hw::read::gs_base(), reinterpret_cast<std::uint64_t>(hw::percpu::self()));
}

KTEST_ALL_CPUS(Hal, fs_base_reads_back_what_was_written, cpu) {
  const std::uint64_t before = hw::read::fs_base();
  const std::uint64_t base = 0x0000'7FFF'0000'0000 + cpu * memory::PAGE_SIZE;

  hw::write::fs_base(base);
  KEXPECT_EQ(hw::read::fs_base(), base);

  hw::write::fs_base(before);
  KEXPECT_EQ(hw::read::fs_base(), before);
}

KTEST(Hal, xsave_variants_are_what_cpuid_says) {
  const CpuInfo& info = *hw::percpu::self()->info;
  if (!info.has<Feature::XSAVE>()) {
    KEXPECT(!info.has<Feature::XSAVEC>());
    KEXPECT(!info.has(Feature::XSS));
    return;
  }

  const auto leaf = CpuInfo::query(0xD, 1);
  KEXPECT(info.has<Feature::XSAVEOPT>() == bit(leaf.eax, 0));
  KEXPECT(info.has<Feature::XSAVEC>() == bit(leaf.eax, 1));
  KEXPECT(info.has<Feature::XSS>() == bit(leaf.eax, 3));
  KEXPECT(info.has<Feature::XFD>() == bit(leaf.eax, 4));

  // The other lookup, the one the static call patcher uses.
  KEXPECT(info.has(Feature::XSAVEC) == bit(leaf.eax, 1));
  KEXPECT(info.has(Feature::XSS) == bit(leaf.eax, 3));
}
}  // namespace kernel::testing
