#include "hal/hal.hpp"

#include "hal/smp/cpu_info.hpp"

namespace kernel::hw {
namespace {
// Whether initialize() turned on CR4.FSGSBASE. The GS base static calls are patched for the BSP,
// so every AP has to match it before its first GS base write.
bool g_fsgsbase = false;

// The FS and GS bases in use right now. GS's SWAPGS partner, IA32_KERNEL_GS_BASE, is 0xC0000102.
constexpr std::uint32_t IA32_FS_BASE = 0xC0000100;
constexpr std::uint32_t IA32_GS_BASE = 0xC0000101;

void idle_generic(const std::atomic<std::uint32_t>* work) noexcept {
  irq::disable();
  if (work->load(std::memory_order_acquire) == 0) wait_for_interrupt();
  irq::enable();
}

void idle_mwait(const std::atomic<std::uint32_t>* work) noexcept {
  irq::disable();

  // Address in RAX; no extensions (ECX) or hints (EDX).
  asm volatile("monitor" : : "a"(work), "c"(0), "d"(0) : "memory");

  // Re-check once the monitor is armed: a store that landed before it would not wake us.
  if (work->load(std::memory_order_acquire) == 0) {
    // C1 (EAX = 0). ECX bit 0 lets a masked interrupt end the wait; it is taken right after the
    // STI below.
    asm volatile("mwait" : : "a"(0), "c"(1) : "memory");
  }

  irq::enable();
}
}  // namespace

void tpause(const std::uint32_t state, const std::uint64_t delay) noexcept {
  std::uint64_t target_tsc = read_tsc() + delay;
  const std::uint32_t target_hi = static_cast<std::uint32_t>(target_tsc >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(target_tsc & 0xFFFFFFFF);

  asm volatile("tpause %%ecx" : : "c"(state), "a"(target_lo), "d"(target_hi) : "memory");
}

DEFINE_STATIC_CALL(cpu_relax_impl, pause);
REGISTER_STATIC_CALL_ALT(cpu_relax_impl, tpause, Feature::WAITPKG);

DEFINE_STATIC_CALL(cpu_idle, idle_generic);
REGISTER_STATIC_CALL_ALT(cpu_idle, idle_mwait, Feature::MONITOR, !Feature::HYPERVISOR);

namespace read {
namespace {
std::uint64_t legacy_gs() noexcept { return msr(IA32_GS_BASE); }

std::uint64_t rdgsbase() noexcept {
  std::uint64_t base;
  asm volatile("rdgsbase %0"
               : "=r"(base)
               : /* No input */
               : /* No Clobber */);
  return base;
}

std::uint64_t legacy_fs() noexcept { return msr(IA32_FS_BASE); }

std::uint64_t rdfsbase() noexcept {
  std::uint64_t base;
  asm volatile("rdfsbase %0"
               : "=r"(base)
               : /* No input */
               : /* No Clobber */);
  return base;
}
}  // namespace

DEFINE_STATIC_CALL(read_gs_base, legacy_gs);
REGISTER_STATIC_CALL_ALT(read_gs_base, rdgsbase, Feature::FSGSBASE);

DEFINE_STATIC_CALL(read_fs_base, legacy_fs);
REGISTER_STATIC_CALL_ALT(read_fs_base, rdfsbase, Feature::FSGSBASE);
}  // namespace read

namespace write {
namespace {
void legacy_gs(const std::uint64_t base) noexcept { msr(IA32_GS_BASE, base); }

void wrgsbase(const std::uint64_t base) noexcept {
  asm volatile("wrgsbase %0"
               : /* No Output */
               : "r"(base)
               : "memory");
}

void legacy_fs(const std::uint64_t base) noexcept { msr(IA32_FS_BASE, base); }

void wrfsbase(const std::uint64_t base) noexcept {
  asm volatile("wrfsbase %0"
               : /* No Output */
               : "r"(base)
               : "memory");
}
}  // namespace

DEFINE_STATIC_CALL(gs_base, legacy_gs);
REGISTER_STATIC_CALL_ALT(gs_base, wrgsbase, Feature::FSGSBASE);

DEFINE_STATIC_CALL(fs_base, legacy_fs);
REGISTER_STATIC_CALL_ALT(fs_base, wrfsbase, Feature::FSGSBASE);
}  // namespace write

void initialize() noexcept {
  g_fsgsbase = profile_manager.get_current()->has<Feature::FSGSBASE>();
  initialize_secondary();
}

void initialize_secondary() noexcept {
  if (g_fsgsbase) {
    write::cr4(read::cr4().with_fsgsbase());
  }
}
}  // namespace kernel::hw
