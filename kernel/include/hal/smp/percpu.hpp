/// @file percpu.hpp
/// @brief The per-CPU area and the GS-relative accessors that reach it.
///
/// Every CPU has one PerCpu, and its GS base points at it, so `percpu::id()` and friends are a
/// single GS-relative load. The BSP's area is static; the APs' are built by the BSP before it
/// starts them (see smp.hpp), which is also where the dense CPU and NUMA IDs come from.
///
/// Data that belongs to a CPU lives here, and data that belongs to a NUMA node in its PerNumaNode,
/// rather than in global tables indexed by ID. That keeps it on the right cache lines, in the
/// right memory, and costs nothing for CPUs the machine does not have.

#pragma once

#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

#include "crypto/random.hpp"
#include "gdt.hpp"
#include "gs.hpp"
#include "hal/apic/lapic.hpp"
#include "hal/timer/timer.hpp"
#include "memory/pmm/pcp_cache.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "sched/dpc.hpp"
#include "sched/numa_domain.hpp"
#include "sched/runqueue.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::sched {
class Thread;
}

namespace kernel::hw {
/// @brief What belongs to a NUMA node as a whole. Lives in memory of that node.
struct alignas(std::hardware_destructive_interference_size) PerNumaNode {
  std::uint32_t numa_id{0};
  sched::NumaDomain scheduler{};
};

class CpuTopology {
  std::uint64_t m_data;

 public:
  constexpr explicit CpuTopology(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, cpu_id, 0, 32)
  BF_RW(std::uint32_t, numa_node, 32, 32)
};

/// @brief Nesting counters of the contexts a CPU can be in, one byte each.
///
/// The layout is fixed: the accessors below read several counters with one GS-relative load.
class CpuContext {
  std::uint64_t m_data;

 public:
  constexpr explicit CpuContext(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, ast_flags, 0, 8)       // Work for the way out of an interrupt
  BF_RW(std::uint8_t, preempt_count, 8, 8)   // Preemption depth
  BF_RW(std::uint8_t, softirq_count, 16, 8)  // Deferred work
  BF_RW(std::uint8_t, hardirq_count, 24, 8)  // IRQ nesting
  BF_RW(std::uint8_t, nmi_count, 32, 8)      // NMI nesting
  BF_RW(std::uint8_t, mce_count, 40, 8)      // Machine Check Exception nesting
};

struct alignas(std::hardware_destructive_interference_size) PerCpu {
  PerCpu* self{this};
  PerNumaNode* numa_node{nullptr};
  const CpuInfo* info{nullptr};
  CpuTopology topology{0};
  memory::VirtualAddress stack_top{};   ///< Kernel stack the CPU was started on.
  sched::Thread* curr_thread{nullptr};  ///< What the CPU is running; see sched::current().

  /// What IA32_KERNEL_GS_BASE holds for the next SWAPGS to user mode, so that a switch only writes
  /// it when it changes. Nothing to begin with, so the first user thread always does.
  std::uint64_t user_gs_base{~0ul};

  /// Top of the kernel stack of the user thread the CPU runs: SYSCALL switches to it. The TSS
  /// holds the same for interrupts, at an offset that straddles a cache line. See hal/syscall.hpp.
  std::uint64_t syscall_stack{0};

  /// Where SYSCALL keeps the user's stack pointer until it has a stack to push it on.
  std::uint64_t syscall_scratch{0};

  gdt::GlobalDescriptorTable gdt_table{};

  alignas(std::hardware_destructive_interference_size) CpuContext context{0};

  /// The CPU's side of RCU: how deep it is in read sections, in the low byte, and above that how
  /// many outermost ones it has finished. Only this CPU writes it, and remote CPUs only read it,
  /// to tell whether a reader they have to wait for is done yet. See utils/rcu/rcu.hpp.
  std::uint64_t rcu_reader{0};

  // Queue nodes for QSpinlock waiters, one per nesting level. Remote CPUs write to these when
  // they queue behind us, so they get a cache line to themselves.
  alignas(std::hardware_destructive_interference_size)
      std::array<utils::McsNode, utils::QSpinlock::MAX_NESTING> qnodes{};

  alignas(std::hardware_destructive_interference_size) memory::vmm::tlb::PcidManager pcid_manager;
  apic::Lapic lapic;
  crypto::CpuRandomState random{};

  timer::TimerQueue timers{};
  sched::RunQueue runqueue{};
  sched::DpcQueue dpcs{};

  constexpr explicit PerCpu() noexcept = default;
};

namespace percpu {
/// @brief Dense ID of the calling CPU: 0 for the BSP, then the APs in boot protocol order.
[[gnu::always_inline]] inline std::uint32_t id() noexcept {
  return READ_PCP(topology).get_cpu_id();
}

/// @brief NUMA node of the calling CPU, as a dense PMM node ID.
[[gnu::always_inline]] inline std::uint32_t numa_node_id() noexcept {
  return READ_PCP(topology).get_numa_node();
}
[[gnu::always_inline]] inline PerCpu* self() noexcept { return READ_PCP(self); }
[[nodiscard]] inline memory::vmm::tlb::PcidManager& pcid_manager() noexcept {
  return self()->pcid_manager;
}
[[nodiscard]] inline apic::Lapic& lapic() noexcept { return self()->lapic; }

/// @brief The calling CPU's timers. Only stays the right one for as long as preemption is off.
[[nodiscard]] inline timer::TimerQueue& timers() noexcept { return self()->timers; }

/// @brief The calling CPU's run queue. Only stays the right one for as long as preemption is off.
[[nodiscard]] inline sched::RunQueue& runqueue() noexcept { return self()->runqueue; }

[[nodiscard]] inline sched::Thread* current_thread() noexcept { return READ_PCP(curr_thread); }
inline void set_current_thread(sched::Thread* thread) noexcept { WRITE_PCP(curr_thread, thread); }

/// @brief Per-CPU area of the CPU with dense ID @p cpu_id (0 is the BSP).
/// @pre The CPU is registered with the SMP code, or is the BSP.
[[nodiscard]] PerCpu* cpu_area(std::uint32_t cpu_id) noexcept;

// CPU context
inline constexpr std::size_t CTX_BASE = offsetof(PerCpu, context);
inline constexpr std::size_t AST_BYTE = CTX_BASE + 0;
inline constexpr std::size_t PREEMPT_BYTE = CTX_BASE + 1;
inline constexpr std::size_t SOFTIRQ_BYTE = CTX_BASE + 2;
inline constexpr std::size_t HARDIRQ_BYTE = CTX_BASE + 3;
inline constexpr std::size_t NMI_BYTE = CTX_BASE + 4;
inline constexpr std::size_t MCE_BYTE = CTX_BASE + 5;

// Bytes 1-5 of the context: the preemption count and every interrupt nesting count. Byte 0 holds
// the AST flags and bytes 6-7 are unused.
inline constexpr std::uint64_t CTX_BLOCKERS = 0x0000'FFFF'FFFF'FF00ul;
inline constexpr std::uint64_t CTX_AST_MASK = 0xFFul;

/// @brief AST flag: the scheduler wants to look at what this CPU should be running.
inline constexpr std::uint8_t AST_NEED_RESCHED = 1u << 0;

/// @brief AST flag: this CPU has deferred calls waiting, see sched/dpc.hpp.
inline constexpr std::uint8_t AST_DPC_PENDING = 1u << 1;

[[nodiscard, gnu::always_inline]] inline std::uint8_t ast_flags() noexcept {
  return gs::read<AST_BYTE, std::uint8_t>();
}

/// @brief Raises AST flags. Safe with interrupts on: a handler may be raising others right now.
[[gnu::always_inline]] inline void or_ast(const std::uint8_t mask) noexcept {
  gs::set_bits<AST_BYTE>(mask);
}

/// @brief Takes AST flags down, and says which of all flags were up.
[[gnu::always_inline]] inline std::uint8_t fetch_clear_ast(const std::uint8_t mask) noexcept {
  return gs::fetch_clear_bits<AST_BYTE>(mask);
}

/// @brief Asks for the running thread to be looked at again as soon as it can lose the CPU.
[[gnu::always_inline]] inline void request_reschedule() noexcept { or_ast(AST_NEED_RESCHED); }

[[gnu::always_inline]] inline void clear_reschedule() noexcept {
  (void)fetch_clear_ast(AST_NEED_RESCHED);
}

/// @brief Whether an AST flag is up, and nothing stands in the way of acting on it.
///
/// Every interrupt and every preempt_enable() looks at this on its way out, so it is one load, and
/// two tests of it.
[[nodiscard, gnu::always_inline]] inline bool ast_pending() noexcept {
  const std::uint64_t context = gs::read<CTX_BASE, std::uint64_t>();
  return (context & CTX_AST_MASK) != 0 && (context & CTX_BLOCKERS) == 0;
}

/// @brief Acts on the AST flags, if interrupts are on. The rare path of preempt_enable().
void preempt_schedule() noexcept;

[[gnu::always_inline]] inline void preempt_disable() noexcept { gs::inc<PREEMPT_BYTE, 1>(); }

/// @brief Ends what preempt_disable() started.
///
/// A reschedule or a deferred call that came up in the meantime happens here. An interrupt could
/// not act on it, and the next one may be a long way off, or a thread that wanted this CPU would
/// be waiting for it. With interrupts off it stays pending, so anything that also restores them
/// does so first.
[[gnu::always_inline]] inline void preempt_enable() noexcept {
  gs::dec<PREEMPT_BYTE, 1>();

  if (ast_pending()) [[unlikely]] {
    preempt_schedule();
  }
}

[[nodiscard, gnu::always_inline]] inline std::uint8_t preempt_count() noexcept {
  return gs::read<PREEMPT_BYTE, std::uint8_t>();
}

[[gnu::always_inline]] inline void softirq_enter() noexcept { gs::inc<SOFTIRQ_BYTE, 1>(); }
[[gnu::always_inline]] inline void softirq_exit() noexcept { gs::dec<SOFTIRQ_BYTE, 1>(); }

[[nodiscard, gnu::always_inline]] inline bool in_softirq() noexcept {
  return gs::read<SOFTIRQ_BYTE, std::uint8_t>() > 0;
}

[[gnu::always_inline]] inline void hardirq_enter() noexcept { gs::inc<HARDIRQ_BYTE, 1>(); }
[[gnu::always_inline]] inline void hardirq_exit() noexcept { gs::dec<HARDIRQ_BYTE, 1>(); }

[[nodiscard, gnu::always_inline]] inline bool in_hardirq() noexcept {
  return gs::read<HARDIRQ_BYTE, std::uint8_t>() > 0;
}

[[gnu::always_inline]] inline void nmi_enter() noexcept { gs::inc<NMI_BYTE, 1>(); }
[[gnu::always_inline]] inline void nmi_exit() noexcept { gs::dec<NMI_BYTE, 1>(); }

[[nodiscard, gnu::always_inline]] inline bool in_nmi() noexcept {
  return gs::read<NMI_BYTE, std::uint8_t>() > 0;
}

[[gnu::always_inline]] inline void mce_enter() noexcept { gs::inc<MCE_BYTE, 1>(); }
[[gnu::always_inline]] inline void mce_exit() noexcept { gs::dec<MCE_BYTE, 1>(); }

[[nodiscard, gnu::always_inline]] inline bool in_mce() noexcept {
  return gs::read<MCE_BYTE, std::uint8_t>() > 0;
}

// The RCU read side. What PerCpu::rcu_reader means, and why entering takes a locked instruction,
// is in utils/rcu/rcu.hpp; use rcu::ReadGuard rather than these.
inline constexpr std::size_t RCU_READER = offsetof(PerCpu, rcu_reader);

/// @brief The part of PerCpu::rcu_reader that counts nested read sections.
inline constexpr std::uint64_t RCU_DEPTH_MASK = 0xFF;

/// @brief What finishing an outermost read section adds to PerCpu::rcu_reader.
inline constexpr std::uint64_t RCU_SECTION_DONE = RCU_DEPTH_MASK + 1;

/// @brief Enters a read section: no preemption, and one more level of nesting.
///
/// The increment is a locked instruction, so a full barrier: the loads of the section cannot be
/// done before the rest of the system can see that this CPU is reading. It is also a single
/// instruction, so an interrupt sees it done or not done, never halfway.
[[gnu::always_inline]] inline void rcu_read_lock() noexcept {
  preempt_disable();
  asm volatile("lock incq %%gs:%c0" : /* No Output */ : "i"(RCU_READER) : "memory", "cc");
}

/// @brief Leaves a read section. The outermost one also counts as finished.
///
/// The load and the update are two instructions. An interrupt in between can only run read
/// sections of its own, which leave the word as they found it, so what was loaded still holds.
/// Preemption comes back the usual way: a reschedule that came up in the section happens here.
[[gnu::always_inline]] inline void rcu_read_unlock() noexcept {
  const std::uint64_t word = gs::read<RCU_READER, std::uint64_t>();
  if ((word & RCU_DEPTH_MASK) == 1) {
    gs::add<RCU_READER, 8, static_cast<std::uint32_t>(RCU_SECTION_DONE - 1)>();
  } else {
    gs::dec<RCU_READER, 8>();
  }

  preempt_enable();
}

/// @brief How many read sections the calling CPU is in, one inside the other.
[[nodiscard, gnu::always_inline]] inline std::uint32_t rcu_depth() noexcept {
  return static_cast<std::uint32_t>(gs::read<RCU_READER, std::uint64_t>() & RCU_DEPTH_MASK);
}

[[nodiscard, gnu::always_inline]] inline bool in_rcu_read() noexcept { return rcu_depth() != 0; }

/// @brief Whether the CPU is handling any interrupt, exception-like NMI/#MC included.
[[nodiscard, gnu::always_inline]] inline bool in_interrupt() noexcept {
  // Softirq, hardirq, NMI and MCE are bytes 2-5: one 32-bit load covers them.
  return gs::read<SOFTIRQ_BYTE, std::uint32_t>() != 0;
}

/// @brief Whether the CPU may block: no preemption disabled and no interrupt in progress.
[[nodiscard, gnu::always_inline]] inline bool can_sleep() noexcept {
  return (gs::read<CTX_BASE, std::uint64_t>() & CTX_BLOCKERS) == 0;
}

/// @brief Sets up the BSP's per-CPU area, GDT and GS base. The first thing the kernel does.
void early_initialize() noexcept;

/// @brief Loads @p cpu's GDT and points GS at it. Run by every CPU on itself.
void install(PerCpu* cpu) noexcept;
}  // namespace percpu
}  // namespace kernel::hw