#include "hal/hal.hpp"

#include "hal/smp/cpu_info.hpp"

namespace kernel::hw {
namespace {
void idle_generic(volatile std::uint32_t* runqueue_count) noexcept {
  while (*runqueue_count == 0) wait_for_interrupt();
}

void idle_mwait(volatile std::uint32_t* runqueue_count) noexcept {
  while (*runqueue_count == 0) {
    asm volatile("monitor"
                 : /* No output */
                 : "a"(runqueue_count) /* Address */, "c"(0) /* Extensions */, "d"(0) /* Hints */
                 : "memory");

    if (*runqueue_count != 0) break;

    asm volatile("mwait"
                 :                                    /* No Output */
                 : "a"(0) /* C1 state hint */, "c"(1) /* Break on interrupts */
                 : "memory");
  }
}
}  // namespace

void pause(std::uint32_t, std::uint64_t) noexcept {
  asm volatile("pause" : /* No Output */ : /* No Input */ : "memory");
}

void tpause(const std::uint32_t state, const std::uint64_t delay) noexcept {
  std::uint64_t target_tsc = read_tsc() + delay;
  const std::uint32_t target_hi = static_cast<std::uint32_t>(target_tsc >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(target_tsc & 0xFFFFFFFF);

  asm volatile("tpause %%ecx" : : "c"(state), "a"(target_lo), "d"(target_hi) : "memory");
}

DEFINE_STATIC_CALL(cpu_relax_impl, pause);
REGISTER_STATIC_CALL_ALT(cpu_relax_impl, tpause, Feature::WAITPKG);

DEFINE_STATIC_CALL(cpu_idle_loop, idle_generic);
REGISTER_STATIC_CALL_ALT(cpu_idle_loop, idle_mwait, Feature::MONITOR, !Feature::HYPERVISOR);

namespace read {
namespace {
std::uint64_t legacy() noexcept {
  constexpr std::uint32_t KERNEL_GS_BASE = 0xC0000101;
  return msr(KERNEL_GS_BASE);
}

std::uint64_t rdgsbase() noexcept {
  std::uint64_t base;
  asm volatile("rdgsbase %0"
               : "=r"(base)
               : /* No input */
               : /* No Clobber */);
  return base;
}
}  // namespace

DEFINE_STATIC_CALL(gs_base, legacy);
REGISTER_STATIC_CALL_ALT(gs_base, rdgsbase, Feature::FSGSBASE);
}  // namespace read

namespace write {
namespace {
void legacy(const std::uint64_t base) noexcept {
  constexpr std::uint32_t KERNEL_GS_BASE = 0xC0000101;
  msr(KERNEL_GS_BASE, base);
}

void _wrgsbase(const std::uint64_t base) noexcept {
  asm volatile("wrgsbase %0"
               : /* No Output */
               : "r"(base)
               : /* No Clobber */);
}
}  // namespace

DEFINE_STATIC_CALL(wrgsbase, legacy);
REGISTER_STATIC_CALL_ALT(wrgsbase, _wrgsbase, Feature::FSGSBASE);
}  // namespace write

void initialize() noexcept {
  const CpuInfo* cpu = profile_manager.get_current();
  if (cpu->has(Feature::FSGSBASE)) write::cr4(read::cr4().with_fsgsbase());
}
}  // namespace kernel::hw