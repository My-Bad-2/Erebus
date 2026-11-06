#pragma once

#include <bitfield.hpp>
#include <cstdint>

#include "memory/address.hpp"
#include "patch/static_call.hpp"

namespace kernel::hw {
inline constexpr uint32_t RELAX_TSC_DELAY = 250;

[[gnu::always_inline]] inline std::uint64_t read_tsc(std::uint32_t* cpu_id) noexcept {
  std::uint32_t cpu, low, high;
  asm volatile("rdtscp"
               : "=a"(low), "=d"(high), "=c"(cpu)
               : /* No Input */
               : "memory");

  if (cpu_id) [[likely]] {
    *cpu_id = cpu;
  }

  return (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
}

[[gnu::always_inline]] inline std::uint64_t read_tsc() noexcept { return read_tsc(nullptr); }

/// @brief One spin-wait hint. The arguments only exist to match cpu_relax_impl.
[[gnu::always_inline]] inline void pause(std::uint32_t = 1,
                                         std::uint64_t = RELAX_TSC_DELAY) noexcept {
  asm volatile("pause" ::: "memory");
}

/// @brief Waits in a light C0 sleep state for @p delay TSC ticks or until an interrupt.
void tpause(std::uint32_t state = 1, std::uint64_t delay = RELAX_TSC_DELAY) noexcept;

DECLARE_STATIC_CALL(void, cpu_relax_impl, std::uint32_t, std::uint64_t);

[[gnu::always_inline]] inline void cpu_relax(const std::uint32_t state = 1,
                                             const std::uint64_t delay = RELAX_TSC_DELAY) noexcept {
  cpu_relax_impl(state, delay);
}

namespace irq {
[[gnu::always_inline]] inline void disable() noexcept { asm volatile("cli" ::: "memory"); }

[[gnu::always_inline]] inline void enable() noexcept { asm volatile("sti" ::: "memory"); }

[[gnu::always_inline]] inline std::uint64_t read_flags() noexcept {
  std::uint64_t rflags;
  asm volatile(
      "pushfq\n\t"
      "popq %0"
      : "=r"(rflags)
      : /* No Input */
      : "memory");
  return rflags;
}

[[gnu::always_inline]] inline void write_flags(const std::uint64_t flags) noexcept {
  asm volatile(
      "pushq %0\n\t"
      "popfq"
      : /* No Output */
      : "r"(flags)
      : "cc", "memory");
}

inline constexpr std::uint64_t RFLAGS_IF = 1ull << 9;

[[gnu::always_inline]] inline bool is_enabled() noexcept { return (read_flags() & RFLAGS_IF) != 0; }

/// @brief Disables interrupts and returns the state to hand to restore().
[[nodiscard, gnu::always_inline]] inline std::uint64_t save() noexcept {
  const std::uint64_t flags = read_flags();
  disable();
  return flags;
}

/// @brief Re-enables interrupts if they were on when @p flags was saved.
///
/// Cheaper than writing RFLAGS back, since POPF is microcoded, and it leaves the other flags
/// alone (AC in particular, which SMAP code toggles on purpose).
[[gnu::always_inline]] inline void restore(const std::uint64_t flags) noexcept {
  if (flags & RFLAGS_IF) {
    enable();
  }
}
}  // namespace irq

/// @brief Enables interrupts and halts until one arrives.
///
/// STI only takes effect after the following instruction, so an interrupt that is already pending
/// cannot slip in between the two and leave the CPU halted with nothing left to wake it. Call
/// with interrupts disabled, after checking whatever condition the caller waits for.
[[gnu::always_inline]] inline void wait_for_interrupt() noexcept {
  asm volatile(
      "sti\n\t"
      "hlt\n\t"
      : /* No output */
      : /* No Input*/
      : "memory");
}

[[gnu::always_inline, noreturn]] inline void dead_loop() noexcept {
  while (true) {
    asm volatile(
        "cli\n\t"
        "hlt\n\t"
        : /* No output */
        : /* No Input*/
        : "memory");
  }
}

/// @brief Idles until `*runqueue_count` is non-zero or an interrupt has been handled.
///
/// Uses MONITOR/MWAIT on bare metal, so a store to the counter wakes the CPU without an IPI, and
/// HLT everywhere else. Returns with interrupts enabled.
DECLARE_STATIC_CALL(void, cpu_idle, const volatile std::uint32_t*);

/// @brief The idle loop of a CPU with nothing to run.
[[noreturn]] void cpu_idle_loop(const volatile std::uint32_t* runqueue_count) noexcept;

/// @brief Stops every other CPU and halts this one, for good. Where panics end up.
[[noreturn]] void panic_halt() noexcept;

[[gnu::always_inline]] inline void umonitor(const volatile void* addr) noexcept {
  asm volatile("umonitor %0" : /* No Output */ : "r"(addr) : "memory");
}

[[gnu::always_inline]] inline void umwait(const std::uint32_t state,
                                          const std::uint64_t tsc_deadline) noexcept {
  const std::uint32_t target_hi = static_cast<std::uint32_t>(tsc_deadline >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(tsc_deadline & 0xFFFFFFFF);

  asm volatile("umwait %2" : : "a"(target_lo), "d"(target_hi), "r"(state) : "cc", "memory");
}

[[gnu::always_inline]] inline void monitorx(const volatile void* addr,
                                            const std::uint32_t extensions,
                                            const std::uint32_t hints) noexcept {
  asm volatile("monitorx"
               : /* No Output */
               : "a"(addr), "c"(extensions), "d"(hints)
               : "memory");
}

[[gnu::always_inline]] inline void mwaitx(const std::uint32_t extension, const std::uint32_t hints,
                                          const std::uint32_t timeout) noexcept {
  asm volatile("mwaitx"
               : /* No Output */
               : "a"(hints), "c"(extension), "b"(timeout)
               : "memory");
}

class CR0 {
  std::uint64_t m_data;

 public:
  constexpr explicit CR0(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(pe, 0)   // protection mode enable
  BF_BIT_RW(mp, 1)   // monitor co-processor
  BF_BIT_RW(em, 2)   // emulation
  BF_BIT_RW(ts, 3)   // task switch
  BF_BIT_RW(et, 4)   // extension type
  BF_BIT_RW(ne, 5)   // numeric error
  BF_BIT_RW(wp, 16)  // write protected
  BF_BIT_RW(am, 18)  // alignment mask
  BF_BIT_RW(nw, 29)  // not write through
  BF_BIT_RW(cd, 30)  // cache disable
  BF_BIT_RW(pg, 31)  // paging
};

class CR4 {
  std::uint64_t m_data;

 public:
  constexpr explicit CR4(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(vme, 0)          // Virtual 8096 mode extensions
  BF_BIT_RW(pvi, 1)          // Protected-mode virtual interrupts
  BF_BIT_RW(tsd, 2)          // Timestamp disabled (except in ring-0)
  BF_BIT_RW(de, 3)           // Debugging extensions
  BF_BIT_RW(pse, 4)          // Page Size extension
  BF_BIT_RW(pae, 5)          // Physical Address Extension
  BF_BIT_RW(mce, 6)          // Machine check exception
  BF_BIT_RW(pge, 7)          // Page global enabled
  BF_BIT_RW(pce, 8)          // Performance-monitoring Counter enable
  BF_BIT_RW(osfxsr, 9)       // Enabled SSE and FPU save & restore
  BF_BIT_RW(osxmmexcpt, 10)  // enables unmasked SSE exceptions
  BF_BIT_RW(umip, 11)
  BF_BIT_RW(la57, 12)      // 5-lvl paging
  BF_BIT_RW(vmxe, 13)      // Virtual machine extensions enable
  BF_BIT_RW(smxe, 14)      // Safer mode extensions enable
  BF_BIT_RW(fsgsbase, 16)  // enables rdfsbase, rdgsbase, wrfsbase, wrgsbase
  BF_BIT_RW(pcide, 17)     // enables pcids
  BF_BIT_RW(osxsave, 18)   // xsave and processor extended states enable
  BF_BIT_RW(kl, 19)        // Key locker enable
  BF_BIT_RW(smep, 20)      // execution of code in a higher ring generates a fault
  BF_BIT_RW(smap, 21)      // access of data in a higher ring generates a fault
  BF_BIT_RW(pke, 22)       // Protection key enable
  BF_BIT_RW(cet, 23)       // Control-flow enforcement
  BF_BIT_RW(pks, 24)       // Enable protection keys for supervisor pages
  BF_BIT_RW(uintr, 25)     // User interrupts enable
  BF_BIT_RW(lass, 27)      // Linear Address space separation
  BF_BIT_RW(lam_sup, 28)   // Linear address masking for supervisor pointers
  BF_BIT_RW(fred, 32)      // Flexible return and event delivery
};

class CR3 {
  std::uint64_t m_data;

 public:
  constexpr explicit CR3(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  // PCID Mode Fields (when CR4.PCIDE = 1)
  BF_RW(std::uint16_t, pcid, 0, 12)  // Process-context identifier
  BF_BIT_RW(no_flush, 63)            // Preserve old TLB entries on CR3 write

  // Legacy Mode Fields (when CR4.PCIDE = 0)
  BF_BIT_RW(pwt, 3)  // Page-level write-through
  BF_BIT_RW(pcd, 4)  // Page-level cache disable

  BF_RW(std::uint64_t, pfn, 12, 40)  // Page frame number

  constexpr memory::PhysicalAddress extract_address() const noexcept {
    return memory::PhysicalAddress{get_pfn() << 12};
  }

  static constexpr CR3 build_legacy(const memory::PhysicalAddress phys, const bool pwt = false,
                                    const bool pcd = false) noexcept {
    CR3 val{0};
    val.set_pfn(phys.value() >> 12);
    val.set_pwt(pwt);
    val.set_pcd(pcd);
    return val;
  }

  static constexpr CR3 build_pcid(const memory::PhysicalAddress phys, const std::uint16_t pcid,
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
  std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(sce, 0)  // enables syscalls
  BF_BIT_RW(lme, 8)  // Enables IA-32e mode operation

  BF_BIT_RO(lma, 10)  // IA-32e mode is active

  BF_BIT_RW(nxe, 11)    // No execute enabled
  BF_BIT_RW(svme, 12)   // enables amd virtualization
  BF_BIT_RW(ffxsr, 14)  // enables optimized versions of fxsave and fxrstor
  BF_BIT_RW(tce, 15)    // enables translation cache extension
};

class XCR0 {
  std::uint64_t m_data;

 public:
  constexpr explicit XCR0(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  static constexpr std::uint64_t XTILECFG = 1ull << 17;
  static constexpr std::uint64_t XTILEDATA = 1ull << 18;

  /// x87, SSE, AVX, AVX-512 and PKRU: everything a thread may have live without asking.
  static constexpr std::uint64_t STANDARD_MASK = 0x2e7;
  /// The same, plus the AMX tile state.
  static constexpr std::uint64_t STANDARD_AMX_MASK = STANDARD_MASK | XTILECFG | XTILEDATA;

  BF_BIT_RW(x87, 0)
  BF_BIT_RW(sse, 1)
  BF_BIT_RW(avx, 2)
  BF_BIT_RW(bndreg, 3)
  BF_BIT_RW(bndcsr, 4)
  BF_BIT_RW(opmask, 5)
  BF_BIT_RW(zmm_hi256, 6)
  BF_BIT_RW(hi16_zmm, 7)
  BF_BIT_RW(processor_trace, 8)
  BF_BIT_RW(pkru, 9)
  BF_BIT_RW(pasid, 10)
  BF_BIT_RW(user_cet, 11)
  BF_BIT_RW(supervisor_cet, 12)
  BF_BIT_RW(hdc, 13)
  BF_BIT_RW(uintr, 14)
  BF_BIT_RW(lbr, 15)
  BF_BIT_RW(hwp, 16)
  BF_BIT_RW(xtilecfg, 17)
  BF_BIT_RW(xtiledata, 18)
  BF_BIT_RW(apx, 19)
  BF_BIT_RW(bsr0, 20)
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

[[gnu::always_inline]] inline XCR0 xcr0() noexcept {
  std::uint32_t eax, edx;
  asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0) : /* No Clobbers */);
  return XCR0{(static_cast<std::uint64_t>(edx) << 32) | eax};
}

DECLARE_STATIC_CALL(std::uint64_t, read_gs_base);
DECLARE_STATIC_CALL(std::uint64_t, read_fs_base);

[[gnu::always_inline]] inline std::uint64_t gs_base() noexcept { return read_gs_base(); }

[[gnu::always_inline]] inline std::uint64_t fs_base() noexcept { return read_fs_base(); }
}  // namespace read

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
  asm volatile("wrmsr"
               : /* No Output */
               : "c"(msr), "a"(lo), "d"(hi)
               : "memory");
}

[[gnu::always_inline]] inline void efer(const EFER val) noexcept {
  constexpr std::uint32_t IA32_EFER = 0xC0000080;
  msr(IA32_EFER, val.raw());
}

[[gnu::always_inline]] inline void xcr0(const XCR0 val) noexcept {
  std::uint32_t eax = static_cast<std::uint32_t>(val.raw());
  std::uint32_t edx = static_cast<std::uint32_t>(val.raw() >> 32);

  asm volatile("xsetbv" : /* No Output */ : "a"(eax), "d"(edx), "c"(0) : /* No Clobbers */);
}

DECLARE_STATIC_CALL(void, gs_base, std::uint64_t);
DECLARE_STATIC_CALL(void, fs_base, std::uint64_t);
}  // namespace write

enum class InvpcidType : std::uint64_t {
  IndividualAddress = 0,
  SingleContext = 1,
  AllContexts = 2,
  AllContextsRetainGlobals = 3
};

struct alignas(16) InvpcidDescriptor {
  std::uint64_t m_data{0};
  BF_RW(std::uint16_t, pcid, 0, 12);
  memory::VirtualAddress address{};

  void set_address(const memory::VirtualAddress addr) noexcept { address = addr; }
};

[[gnu::always_inline]] inline void invpcid(InvpcidType type,
                                           const InvpcidDescriptor& desc) noexcept {
  asm volatile("invpcid %1, %0"
               : /* No Output */
               : "r"(std::to_underlying(type)), "m"(desc)
               : "memory");
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

  // Layout per the AMD APM (INVLPGB): there is no separate "global only" bit.
  BF_BIT_RW(va_valid, 0)        // Flush specific VA
  BF_BIT_RW(pcid_valid, 1)      // Match the PCID
  BF_BIT_RW(asid_valid, 2)      // Match the ASID
  BF_BIT_RW(include_global, 3)  // Also flush global translations
  BF_BIT_RW(final_only, 4)      // Flush only the leaf PTE, keep paging-structure caches
  BF_BIT_RW(include_nested, 5)  // Flush guest hypervisor translations

  BF_RW(std::uint64_t, virt_addr, 12, 52)  // Target Virtual Address
};

// Maps to EDX
class InvlpgbContext {
  std::uint32_t m_data;

 public:
  constexpr explicit InvlpgbContext(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint16_t, asid, 0, 16)   // Guest Address Space ID
  BF_RW(std::uint16_t, pcid, 16, 12)  // Process-Context ID
};

// Maps to ECX
class InvlpgbCount {
  std::uint32_t m_data;

 public:
  constexpr explicit InvlpgbCount(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(std::uint16_t, extra_count, 0, 16)  // No. of additional pages to flush
  BF_BIT_RW(large_page_stride, 31)          // Increment VA by 2MB/1GB per count
};

[[gnu::always_inline]] inline void invlpgb(InvlpgbAddress rax, InvlpgbCount ecx,
                                           InvlpgbContext edx) noexcept {
  asm volatile("invlpgb"
               : /* No Output */
               : "a"(rax.raw()), "c"(ecx.raw()), "d"(edx.raw())
               : "memory");
}

[[gnu::always_inline]] inline void tlbsync() noexcept {
  asm volatile("tlbsync" : /* No Output */ : /* No Input */ : "memory");
}

/// @brief Switches to the stack at @p new_stack_top and calls `target_function(payload)` there.
///
/// The old stack is abandoned and the target must not return. The return address slot is left
/// empty (a zero frame pointer ends backtraces), and the stack is set up the way a `call` would
/// leave it, so the target sees the alignment the ABI promises.
[[noreturn, gnu::naked, gnu::noinline]] inline void execute_on_new_stack(
    std::uint64_t new_stack_top, void* payload, void (*target_function)(void*)) noexcept {
  asm("andq $-16, %rdi\n\t"
      "movq %rdi, %rsp\n\t"
      "movq %rsi, %rdi\n\t"
      "xorl %ebp, %ebp\n\t"
      "pushq $0\n\t"
      "jmp *%rdx\n\t");
}

/// @brief Turns on the BSP's CPU features that do not belong to a particular subsystem.
void initialize() noexcept;

/// @brief Turns on the same features on an AP. Runs before the AP has a GS base.
void initialize_secondary() noexcept;
}  // namespace kernel::hw