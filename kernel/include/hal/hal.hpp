#pragma once

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

class CR0 {
  std::uint64_t m_data;

public:
  constexpr explicit CR0(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(pe, 0)  // protection mode enable
  BF_BIT_RW(mp, 1)  // monitor co-processor
  BF_BIT_RW(em, 2)  // emulation
  BF_BIT_RW(ts, 3)  // task switch
  BF_BIT_RW(et, 4)  // extension type
  BF_BIT_RW(ne, 5)  // numeric error
  BF_BIT_RW(wp, 16) // write protected
  BF_BIT_RW(am, 18) // alignment mask
  BF_BIT_RW(nw, 29) // not write through
  BF_BIT_RW(cd, 30) // cache disable
  BF_BIT_RW(pg, 31) // paging
};

class CR4 {
  std::uint64_t m_data;

public:
  constexpr explicit CR4(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(vme, 0)         // Virtual 8096 mode extensions
  BF_BIT_RW(pvi, 1)         // Protected-mode virtual interrupts
  BF_BIT_RW(tsd, 2)         // Timestamp disabled (except in ring-0)
  BF_BIT_RW(de, 3)          // Debugging extensions
  BF_BIT_RW(pse, 4)         // Page Size extension
  BF_BIT_RW(pae, 5)         // Physical Address Extension
  BF_BIT_RW(mce, 6)         // Machine check exception
  BF_BIT_RW(pge, 7)         // Page global enabled
  BF_BIT_RW(pce, 8)         // Performance-monitoring Counter enable (in any privilege level)
  BF_BIT_RW(osfxsr, 9)      // Enabled SSE and FPU save & restore
  BF_BIT_RW(osxmmexcpt, 10) // enables unmasked SSE exceptions
  BF_BIT_RW(umip, 11)       // sgdt, sidt, sldt, smsw, and str can't be used if cpl > 0.
  BF_BIT_RW(la57, 12)       // 5-lvl paging
  BF_BIT_RW(vmxe, 13)       // Virtual machine extensions enable
  BF_BIT_RW(smxe, 14)       // Safer mode extensions enable
  BF_BIT_RW(fsgsbase, 16)   // enables rdfsbase, rdgsbase, wrfsbase, wrgsbase
  BF_BIT_RW(pcide, 17)      // enables pcids
  BF_BIT_RW(osxsave, 18)    // xsave and processor extended states enable
  BF_BIT_RW(kl, 19)         // Key locker enable
  BF_BIT_RW(smep, 20)       // execution of code in a higher ring generates a fault
  BF_BIT_RW(smap, 21)       // access of data in a higher ring generates a fault
  BF_BIT_RW(pke, 22)        // Protection key enable
  BF_BIT_RW(cet, 23)        // Control-flow enforcement
  BF_BIT_RW(pks, 24)        // Enable protection keys for supervisor pages
  BF_BIT_RW(uintr, 25)      // User interrupts enable
  BF_BIT_RW(lass, 27)       // Linear Address space separation
  BF_BIT_RW(lam_sup, 28)    // Linear address masking for supervisor pointers
  BF_BIT_RW(fred, 32)       // Flexible return and event delivery
};

class CR3 {
  std::uint64_t m_data;

public:
  constexpr explicit CR3(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  // PCID Mode Fields (when CR4.PCIDE = 1)
  BF_RW(std::uint16_t, pcid, 0, 12) // Process-context identifier
  BF_BIT_RW(no_flush, 63)           // Preserve old TLB entries on CR3 write

  // Legacy Mode Fields (when CR4.PCIDE = 0)
  BF_BIT_RW(pwt, 3) // Page-level write-through
  BF_BIT_RW(pcd, 4) // Page-level cache disable

  BF_RW(std::uint64_t, pfn, 12, 40) // Page frame number (base physical address)

  [[nodiscard]] constexpr memory::PhysicalAddress extract_address() const noexcept {
    return memory::PhysicalAddress{get_pfn() << 12};
  }

  [[nodiscard]] static constexpr CR3 build_legacy(const memory::PhysicalAddress phys, const bool pwt = false,
                                                  const bool pcd = false) noexcept {
    CR3 val{0};
    val.set_pfn(phys.value() >> 12);
    val.set_pwt(pwt);
    val.set_pcd(pcd);
    return val;
  }

  [[nodiscard]] static constexpr CR3 build_pcid(const memory::PhysicalAddress phys, const std::uint16_t pcid,
                                                const bool no_flush = false) noexcept {
    CR3 val{0};
    val.set_pfn(phys.value() >> 12);
    val.set_pcid(pcid & 0xfff);
    val.set_no_flush(no_flush);
    return val;
  }
};

class EFER {
  std::uint64_t m_data;

public:
  constexpr explicit EFER(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(sce, 0) // enables syscalls
  BF_BIT_RW(lme, 8) // Enables IA-32e mode operation

  BF_BIT_RO(lma, 10) // IA-32e mode is active

  BF_BIT_RW(nxe, 11)   // No execute enabled
  BF_BIT_RW(svme, 12)  // enables amd virtualization
  BF_BIT_RW(ffxsr, 14) // enables optimized versions of fxsave and fxrstor
  BF_BIT_RW(tce, 15)   // enables translation cache extension
};

namespace read {
[[gnu::always_inline]] inline CR0 cr0() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr0, %0" : "=r"(val) : /* No Input */ : "memory");
  return CR0{val};
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

[[gnu::always_inline]] inline CR4 cr4() noexcept {
  std::uint64_t val;
  asm volatile("mov %%cr4, %0" : "=r"(val) : /* No Input */ : "memory");
  return CR4{val};
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

[[gnu::always_inline]] inline EFER efer() noexcept {
  constexpr std::uint32_t IA32_EFER = 0xC0000080;
  return EFER{msr(IA32_EFER)};
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
[[gnu::always_inline]] inline void cr0(const CR0 val) noexcept {
  asm volatile("mov %0, %%cr0" : /* No output */ : "r"(val.raw()) : "memory");
}

[[gnu::always_inline]] inline void cr2(const std::uint64_t val) noexcept {
  asm volatile("mov %0, %%cr2" : /* No output */ : "r"(val) : "memory");
}

[[gnu::always_inline]] inline void cr3(CR3 val) noexcept {
  asm volatile("mov %0, %%cr3" : /* No output */ : "r"(val.raw()) : "memory");
}

[[gnu::always_inline]] inline void cr4(const CR4 val) noexcept {
  asm volatile("mov %0, %%cr4" : /* No output */ : "r"(val.raw()) : "memory");
}

[[gnu::always_inline]] inline void cr8(const std::uint64_t val) noexcept {
  asm volatile("mov %0, %%cr8" : /* No output */ : "r"(val) : "memory");
}

[[gnu::always_inline]] inline void msr(const std::uint32_t msr, const std::uint64_t val) noexcept {
  const std::uint32_t lo = val & 0xFFFFFFFF;
  const std::uint32_t hi = val >> 32;
  asm volatile("wrmsr" : /* No Output */ : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

[[gnu::always_inline]] inline void efer(const EFER val) noexcept {
  constexpr std::uint32_t IA32_EFER = 0xC0000080;
  msr(IA32_EFER, val.raw());
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

// Maps to RAX
class InvlpgbAddress {
  std::uint64_t m_data;

public:
  constexpr explicit InvlpgbAddress(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(va_valid, 0)       // Flush specific VA
  BF_BIT_RW(pcid_valid, 1)     // Match the PCID
  BF_BIT_RW(asid_valid, 2)     // Match the ASID
  BF_BIT_RW(global, 3)         // Flush Global pages
  BF_BIT_RW(include_global, 4) // Flush pcid/asid and global
  BF_BIT_RW(final_only, 5)     // Flush only the leaf PTE
  BF_BIT_RW(include_nested, 6) // Flush guest hypervisor translations

  BF_RW(std::uint64_t, virt_addr, 12, 52) // Target Virtual Address (bits 12-63)
};

// Maps to EDX
class InvlpgbContext {
  std::uint32_t m_data;

public:
  constexpr explicit InvlpgbContext(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint16_t, asid, 0, 16)  // Guest Address Space ID
  BF_RW(std::uint16_t, pcid, 16, 12) // Process-Context ID
};

// Maps to ECX
class InvlpgbCount {
  std::uint32_t m_data;

public:
  constexpr explicit InvlpgbCount(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint16_t, extra_count, 0, 16) // Number of additional pages to flush
  BF_BIT_RW(large_page_stride, 31)         // Increment VA by 2MB/1GB per count
};

[[gnu::always_inline]] inline void invlpgb(InvlpgbAddress rax, InvlpgbCount ecx, InvlpgbContext edx) noexcept {
  asm volatile("invlpgb"
               : /* No Output */
               : "a"(rax.raw()), "c"(ecx.raw()), "d"(edx.raw())
               : "memory");
}

[[gnu::always_inline]] inline void tlbsync() noexcept {
  asm volatile("tlbsync" : /* No Output */ : /* No Input */ : "memory");
}

void initialize() noexcept;
} // namespace kernel::hw