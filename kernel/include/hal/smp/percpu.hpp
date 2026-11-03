/// @file percpu.hpp
/// @brief The per-CPU area and the GS-relative accessors that reach it.
///
/// Every CPU has one PerCpu, and its GS base points at it, so `percpu::id()` and friends are a
/// single GS-relative load. The BSP's area is static; the APs' are built by the BSP before it
/// starts them (see smp.hpp), which is also where the dense CPU and NUMA IDs come from.

#pragma once

#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

#include "crypto/random.hpp"
#include "gdt.hpp"
#include "gs.hpp"
#include "hal/apic/lapic.hpp"
#include "memory/pmm/pcp_cache.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::hw {
struct alignas(std::hardware_destructive_interference_size) PerNumaNode {
  std::uint32_t numa_id;
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

  BF_RW(std::uint8_t, ast_flags, 0, 8)       // Asynchronous traps count
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
  memory::VirtualAddress stack_top{};  ///< Kernel stack the CPU was started on.
  gdt::GlobalDescriptorTable gdt_table{};

  alignas(std::hardware_destructive_interference_size) CpuContext context{0};

  // Queue nodes for QSpinlock waiters, one per nesting level. Remote CPUs write to these when
  // they queue behind us, so they get a cache line to themselves.
  alignas(std::hardware_destructive_interference_size)
      std::array<utils::McsNode, utils::QSpinlock::MAX_NESTING> qnodes{};

  alignas(std::hardware_destructive_interference_size) memory::vmm::tlb::PcidManager pcid_manager;
  apic::Lapic lapic;
  crypto::CpuRandomState random{};

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

[[nodiscard, gnu::always_inline]] inline std::uint8_t ast_flags() noexcept {
  return gs::read<AST_BYTE, std::uint8_t>();
}

[[gnu::always_inline]] inline void set_ast(const std::uint8_t flags) noexcept {
  gs::write<AST_BYTE, std::uint8_t>(flags);
}

[[gnu::always_inline]] inline void preempt_disable() noexcept { gs::inc<PREEMPT_BYTE, 1>(); }
[[gnu::always_inline]] inline void preempt_enable() noexcept { gs::dec<PREEMPT_BYTE, 1>(); }

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

/// @brief Whether the CPU is handling any interrupt, exception-like NMI/#MC included.
[[nodiscard, gnu::always_inline]] inline bool in_interrupt() noexcept {
  // Softirq, hardirq, NMI and MCE are bytes 2-5: one 32-bit load covers them.
  return gs::read<SOFTIRQ_BYTE, std::uint32_t>() != 0;
}

/// @brief Whether the CPU may block: no preemption disabled and no interrupt in progress.
[[nodiscard, gnu::always_inline]] inline bool can_sleep() noexcept {
  // Bytes 1-5. Byte 0 holds the AST flags and bytes 6-7 are unused.
  constexpr std::uint64_t BLOCKERS = 0x0000'FFFF'FFFF'FF00ull;
  return (gs::read<CTX_BASE, std::uint64_t>() & BLOCKERS) == 0;
}

/// @brief Sets up the BSP's per-CPU area, GDT and GS base. The first thing the kernel does.
void early_initialize() noexcept;

/// @brief Loads @p cpu's GDT and points GS at it. Run by every CPU on itself.
void install(PerCpu* cpu) noexcept;
}  // namespace percpu
}  // namespace kernel::hw