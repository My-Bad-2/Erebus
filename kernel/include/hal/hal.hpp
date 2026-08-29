#pragma once

#include "gs.hpp"
#include "patch.h"
#include "patcher.hpp"

#include "memory/address.hpp"
#include <bitfield.hpp>
#include <cstdint>

namespace kernel::hw {
namespace detail {
inline constexpr uint32_t RELAX_TSC_DELAY = 250;
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_waitpkg = {false};
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_moniterx = {false};
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_mwait = {false};
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_fsgsbase = {false};
} // namespace detail

[[gnu::always_inline]] inline std::uint64_t read_tsc(std::uint32_t *cpu_id) noexcept {
  std::uint32_t cpu, low, high;
  asm volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(cpu) : /* No Input */ : "memory");

  if (cpu_id) [[likely]] {
    *cpu_id = cpu;
  }

  return (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
}

[[gnu::always_inline]] inline std::uint64_t read_tsc() noexcept { return read_tsc(nullptr); }

[[gnu::always_inline]] inline void pause() noexcept {
  asm volatile("pause" : /* No Output */ : /* No Input */ : "memory");
}

[[gnu::always_inline]] inline void tpause(const std::uint32_t state = 1,
                                          const std::uint64_t delay = detail::RELAX_TSC_DELAY) noexcept {
  std::uint64_t target_tsc = read_tsc() + delay;
  const std::uint32_t target_hi = static_cast<std::uint32_t>(target_tsc >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(target_tsc & 0xFFFFFFFF);

  asm volatile("tpause %%ecx" : : "c"(state), "a"(target_lo), "d"(target_hi) : "memory");
}

[[gnu::always_inline]] inline void cpu_relax(const std::uint32_t state = 1,
                                             const std::uint64_t delay = detail::RELAX_TSC_DELAY) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_waitpkg)) {
    tpause(state, delay);
  } else {
    pause();
  }
}

namespace irq {
[[gnu::always_inline]] inline void disable() noexcept { asm volatile("cli" ::: "memory"); }
[[gnu::always_inline]] inline void enable() noexcept { asm volatile("sti" ::: "memory"); }
[[gnu::always_inline]] inline std::uint64_t read_flags() noexcept {
  std::uint64_t rflags;
  asm volatile("pushfq\n\t"
               "popq %0"
               : "=r"(rflags)
               : /* No Input */
               : "memory");
  return rflags;
}

[[gnu::always_inline]] inline void write_flags(const std::uint64_t flags) noexcept {
  asm volatile("pushq %0\n\t"
               "popfq"
               : /* No Output */
               : "r"(flags)
               : "cc", "memory");
}

[[gnu::always_inline]] inline bool is_enabled() noexcept { return (read_flags() & (1UL << 9)) != 0; }
} // namespace irq

[[gnu::always_inline]] inline void wait_for_interrupt() noexcept {
  while (true) {
    asm volatile("sti\n\t"
                 "hlt\n\t"
                 : /* No output */
                 : /* No Input*/
                 : "memory");
  }
}

[[gnu::always_inline, noreturn]] inline void dead_loop() noexcept {
  while (true) {
    asm volatile("cli\n\t"
                 "hlt\n\t"
                 : /* No output */
                 : /* No Input*/
                 : "memory");
  }
}

namespace detail {
[[gnu::always_inline]] inline void idle_generic(volatile std::uint32_t *runqueue_count) noexcept {
  while (*runqueue_count == 0) {
    wait_for_interrupt();
  }
}

[[gnu::always_inline]] inline void idle_mwait(volatile std::uint32_t *runqueue_count) noexcept {
  while (*runqueue_count == 0) {
    asm volatile("monitor"
                 :                                                                    /* No output */
                 : "a"(runqueue_count) /* Address */, "c"(0) /* Extensions */, "d"(0) /* Hints */
                 : "memory");

    if (*runqueue_count != 0) {
      break;
    }

    asm volatile("mwait" : /* No Output */ : "a"(0) /* C1 state hint */, "c"(1) /* Break on interrupts */ : "memory");
  }
}
} // namespace detail

[[gnu::always_inline]] inline void cpu_idle_loop(volatile std::uint32_t *runqueue_count) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_mwait)) {
    detail::idle_mwait(runqueue_count);
  } else {
    detail::idle_generic(runqueue_count);
  }
}

[[gnu::always_inline]] inline void umonitor(const volatile void *addr) noexcept {
  asm volatile("umonitor %0" : /* No Output */ : "r"(addr) : "memory");
}

[[gnu::always_inline]] inline void umwait(const std::uint32_t state, const std::uint64_t tsc_deadline) noexcept {
  const std::uint32_t target_hi = static_cast<std::uint32_t>(tsc_deadline >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(tsc_deadline & 0xFFFFFFFF);

  asm volatile("umwait %2" : : "a"(target_lo), "d"(target_hi), "r"(state) : "cc", "memory");
}

[[gnu::always_inline]] inline void monitorx(const volatile void *addr, const std::uint32_t extensions,
                                            const std::uint32_t hints) noexcept {
  asm volatile("monitorx" : /* No Output */ : "a"(addr), "c"(extensions), "d"(hints) : "memory");
}

[[gnu::always_inline]] inline void mwaitx(const std::uint32_t extension, const std::uint32_t hints,
                                          const std::uint32_t timeout) noexcept {
  asm volatile("mwaitx" : /* No Output */ : "a"(hints), "c"(extension), "b"(timeout) : "memory");
}

using CR0Schema = klib::BitfieldSchema<klib::Bit<"pe", 0>,  // Protected Mode Enable
                                       klib::Bit<"mp", 1>,  // Monitor co-processor
                                       klib::Bit<"em", 2>,  // Emulation
                                       klib::Bit<"ts", 3>,  // Task Switched
                                       klib::Bit<"et", 4>,  // Extension type
                                       klib::Bit<"ne", 5>,  // numeric error
                                       klib::Bit<"wp", 16>, // write protect
                                       klib::Bit<"am", 18>, // Alignment Mask
                                       klib::Bit<"nw", 29>, // Not-Write through
                                       klib::Bit<"cd", 30>, // Cache disable
                                       klib::Bit<"pg", 31>, // Paging
                                       klib::Reserved<32, 32>>;

using CR4Schema =
    klib::BitfieldSchema<klib::Bit<"vme", 0>,         // Virtual 8096 mode extensions
                         klib::Bit<"pvi", 1>,         // Protected-mode virtual interrupts
                         klib::Bit<"tsd", 2>,         // Timestamp disabled (except in ring-0)
                         klib::Bit<"de", 3>,          // Debugging extensions
                         klib::Bit<"pse", 4>,         // Page Size extension
                         klib::Bit<"pae", 5>,         // Physical Address Extension
                         klib::Bit<"mce", 6>,         // Machine check exception
                         klib::Bit<"pge", 7>,         // Page global enabled
                         klib::Bit<"pce", 8>,         // Performance-monitoring Counter enable (in any privilege level)
                         klib::Bit<"osfxsr", 9>,      // Enabled SSE and FPU save & restore
                         klib::Bit<"osxmmexcpt", 10>, // enables unmasked SSE exceptions
                         klib::Bit<"umip", 11>,       // sgdt, sidt, sldt, smsw, and str can't be used if cpl > 0.
                         klib::Bit<"la57", 12>,       // 5-lvl paging
                         klib::Bit<"vmxe", 13>,       // Virtual machine extensions enable
                         klib::Bit<"smxe", 14>,       // Safer mode extensions enable
                         klib::Bit<"fsgsbase", 16>,   // enables rdfsbase, rdgsbase, wrfsbase, wrgsbase
                         klib::Bit<"pcide", 17>,      // enables pcids
                         klib::Bit<"osxsave", 18>,    // xsave and processor extended states enable
                         klib::Bit<"kl", 19>,         // Key locker enable
                         klib::Bit<"smep", 20>,       // execution of code in a higher ring generates a fault
                         klib::Bit<"smap", 21>,       // access of data in a higher ring generates a fault
                         klib::Bit<"pke", 22>,        // Protection key enable
                         klib::Bit<"cet", 23>,        // Control-flow enforcement
                         klib::Bit<"pks", 24>,        // Enable protection keys for supervisor pages
                         klib::Bit<"uintr", 25>,      // User interrupts enable
                         klib::Bit<"lass", 27>,       // Linear Address space separation
                         klib::Bit<"lam-sup", 28>,    // Linear address masking for supervisor pointers
                         klib::Bit<"fred", 32>        // Flexible return and event delivery
                         >;

using CR3LegacySchema = klib::BitfieldSchema<klib::Reserved<0, 3>, klib::Bit<"pwt", 3>, klib::Bit<"pcd", 4>,
                                             klib::Reserved<5, 7>, klib::Field<"pfn", 12, 40>>;

using CR3PcidSchema =
    klib::BitfieldSchema<klib::Field<"pcid", 0, 12>, klib::Field<"pfn", 12, 40>, klib::Bit<"no-flush", 63>>;

struct CR3 {
  std::uint64_t raw{0};

  constexpr CR3() noexcept = default;
  constexpr explicit CR3(const std::uint64_t val) noexcept : raw{val} {}

  constexpr operator std::uint64_t() const noexcept { return raw; }

  [[nodiscard]] constexpr CR3LegacySchema legacy() const noexcept { return CR3LegacySchema{raw}; }
  [[nodiscard]] constexpr CR3PcidSchema pcid() const noexcept { return CR3PcidSchema{raw}; }

  [[nodiscard]] constexpr memory::PhysicalAddress extract_address() const noexcept {
    return memory::PhysicalAddress{legacy().get<"pfn">() << 12};
  }

  [[nodiscard]] static constexpr CR3 build_legacy(const memory::PhysicalAddress phys, const bool pwt = false,
                                                  const bool pcd = false) noexcept {
    CR3LegacySchema schema{0};
    schema.set_mut<"pfn">(phys.value() >> 12);
    schema.set_mut<"pwt">(pwt ? 1 : 0);
    schema.set_mut<"pcd">(pcd ? 1 : 0);

    return CR3{static_cast<std::uint64_t>(schema)};
  }

  [[nodiscard]] static constexpr CR3 build_pcid(const memory::PhysicalAddress phys, const std::uint16_t pcid,
                                                const bool no_flush = false) noexcept {
    CR3PcidSchema schema{0};
    schema.set_mut<"pfn">(phys.value() >> 12);
    schema.set_mut<"pcid">(pcid & 0xFFF);
    schema.set_mut<"no-flush">(no_flush ? 1 : 0);
    return CR3{static_cast<std::uint64_t>(schema)};
  }
};

using EferSchema = klib::BitfieldSchema<klib::Bit<"sce", 0>,       // enables syscalls
                                        klib::Bit<"lme", 8>,       // Enables IA-32e mode operation
                                        klib::ReadOnly<"lma", 10>, // IA-32e mode is active
                                        klib::Bit<"nxe", 11>,      // No execute enabled
                                        klib::Bit<"svme", 12>,     // enables amd virtualization
                                        klib::Bit<"ffxsr", 14>,    // enables optimized versions of fxsave and fxrstor
                                        klib::Bit<"tce", 15>,      // enables translation cache extension
                                        klib::Reserved<16, 47>>;

namespace read {
[[gnu::always_inline]] inline CR0Schema cr0() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr0, %0" : "=r"(val) : /* No Input */ : "memory");
  return CR0Schema{val};
}

[[gnu::always_inline]] inline std::uint64_t cr2() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr2, %0" : "=r"(val) : /* No Input */ : "memory");
  return val;
}

[[gnu::always_inline]] inline CR3 cr3() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr3, %0" : "=r"(val) : /* No Input */ : "memory");
  return CR3{val};
}

[[gnu::always_inline]] inline CR4Schema cr4() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr4, %0" : "=r"(val) : /* No Input */ : "memory");
  return CR4Schema{val};
}

[[gnu::always_inline]] inline std::uint64_t cr8() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr8, %0" : "=r"(val) : /* No Input */ : "memory");
  return val;
}

[[gnu::always_inline]] inline std::uint64_t msr(const std::uint32_t msr) noexcept {
  std::uint32_t lo, hi;
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
  return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

[[gnu::always_inline]] inline EferSchema efer() noexcept {
  constexpr std::uint32_t IA32_EFER = 0xC0000080;
  return EferSchema{msr(IA32_EFER)};
}

[[gnu::always_inline]] inline std::uint64_t gs_base() noexcept {
  std::uint64_t base;
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_fsgsbase)) {
    asm volatile("wrgsbase %0" : "=r"(base) : /* No input */ : /* No Clobber */);
  } else {
    constexpr std::uint32_t KERNEL_GS_BASE = 0xC0000101;
    base = msr(KERNEL_GS_BASE);
  }

  return base;
}
} // namespace read

namespace write {
[[gnu::always_inline]] inline void cr0(const CR0Schema val) noexcept {
  asm volatile("mov %0, %%cr0" : /* No output */ : "r"(val.data) : "memory");
}

[[gnu::always_inline]] inline void cr2(const std::uint64_t val) noexcept {
  asm volatile("mov %0, %%cr2" : /* No output */ : "r"(val) : "memory");
}

[[gnu::always_inline]] inline void cr3(CR3 val) noexcept {
  asm volatile("mov %0, %%cr3" : /* No output */ : "r"(static_cast<std::uint64_t>(val)) : "memory");
}

[[gnu::always_inline]] inline void cr4(const CR4Schema val) noexcept {
  asm volatile("mov %0, %%cr4" : /* No output */ : "r"(val.data) : "memory");
}

[[gnu::always_inline]] inline void cr8(const std::uint64_t val) noexcept {
  asm volatile("mov %0, %%cr8" : /* No output */ : "r"(val) : "memory");
}

[[gnu::always_inline]] inline void msr(const std::uint32_t msr, const std::uint64_t val) noexcept {
  const std::uint32_t lo = val & 0xFFFFFFFF;
  const std::uint32_t hi = val >> 32;
  asm volatile("wrmsr" : /* No Output */ : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

[[gnu::always_inline]] inline void efer(const EferSchema val) noexcept {
  constexpr std::uint32_t IA32_EFER = 0xC0000080;
  msr(IA32_EFER, val.data);
}

[[gnu::always_inline]] inline void gs_base(const std::uint64_t base) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_fsgsbase)) {
    asm volatile("wrgsbase %0" : /* No Output */ : "r"(base) : /* No Clobber */);
  } else {
    constexpr std::uint32_t KERNEL_GS_BASE = 0xC0000101;
    msr(KERNEL_GS_BASE, base);
  }
}
} // namespace write

enum class InvpcidType : std::uint64_t {
  IndividualAddress = 0,
  SingleContext = 1,
  AllContexts = 2,
  AllContextsRetainGlobals = 3
};

struct alignas(16) InvpcidDescriptor {
  std::uint64_t pcid : 12;
  std::uint64_t reserved : 52;
  std::uint64_t address;
};

[[gnu::always_inline]] inline void invpcid(InvpcidType type, const InvpcidDescriptor &desc) noexcept {
  asm volatile("invpcid %1, %0" : /* No Output */ : "r"(std::to_underlying(type)), "m"(desc) : "memory");
}

[[gnu::always_inline]] inline void invlpg(std::uintptr_t addr) noexcept {
  asm volatile("invlpg (%0)" : /* No Output */ : "r"(addr) : "memory");
}

using InvlpgbAddressFlagsSchema =
    klib::BitfieldSchema<klib::Bit<"va_valid", 0>,                              // Flush specific VA
                         klib::Bit<"pcid_valid", 1>,                            // Match the PCID.
                         klib::Bit<"asid_valid", 2>,                            // Match the ASID.
                         klib::Bit<"global", 3>,                                // Flush Global pages.
                         klib::Bit<"include_global", 4>,                        // Flush pcid/asid and global
                         klib::Bit<"final_only", 5>,                            // Flush only the leaf PTE.
                         klib::Bit<"include_nested", 6>,                        // Flush guest hypervisor translations.
                         klib::Reserved<7, 5>, klib::Field<"virt_addr", 12, 52> // Target Virtual Address.
                         >;

using InvlpgbContextIdsSchema =
    klib::BitfieldSchema<klib::Field<"asid", 0, 16>,  // Guest Address Space ID (Virtualization only)
                         klib::Field<"pcid", 16, 12>, // Process-Context ID (Matches CR3 PCID).
                         klib::Reserved<28, 4>>;

using InvlpgbPageCountSchema =
    klib::BitfieldSchema<klib::Field<"extra_count", 0, 16>, // number of additional pages to flush
                         klib::Reserved<16, 15>, klib::Bit<"large_page_stride", 31> // increment va by 2mb/1gb per count
                         >;

[[gnu::always_inline]] inline void invlpgb(InvlpgbAddressFlagsSchema rax, InvlpgbPageCountSchema ecx,
                                           InvlpgbContextIdsSchema edx) noexcept {
  asm volatile("invlpgb" : /* No Output */ : "a"(rax.data), "c"(ecx.data), "d"(edx.data) : "memory");
}

[[gnu::always_inline]] inline void tlbsync() noexcept {
  asm volatile("tlbsync" : /* No Output */ : /* No Input */ : "memory");
}

void initialize() noexcept;
} // namespace kernel::hw