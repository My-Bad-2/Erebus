#pragma once

#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

#include "gs.hpp"
#include "memory/pmm/pcp_cache.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::hw {
using CpuTopology = klib::BitfieldSchema<klib::Field<"cpu-id", 0, 32>,    // 32 bits: CPU Cores
                                         klib::Field<"numa-node", 32, 32> // 32 bits: NUMA Nodes
                                         >;

using CpuContext = klib::BitfieldSchema<klib::Field<"ast-flags", 0, 8>,      // Asynchronous traps count
                                        klib::Field<"preempt-count", 8, 8>,  // Preemption depth
                                        klib::Field<"softirq-count", 16, 8>, // deferred work
                                        klib::Field<"hardirq-count", 24, 8>, // IRQ nesting
                                        klib::Field<"nmi-count", 32, 4>,     // NMI nesting
                                        klib::Field<"mce-count", 36, 4>      // Machine Check Exception nesting
                                        >;

struct alignas(std::hardware_destructive_interference_size) PerCpu {
  PerCpu *self{this};
  CpuTopology topology{0};
  CpuContext context{0};

  const CpuInfo *info;

  std::array<utils::CLHNode, 2> node;
  utils::CLHNode *curr_node{&node[0]};
  utils::CLHNode *prev_node{&node[1]};

  constexpr explicit PerCpu() noexcept = default;
};

namespace percpu {
[[gnu::always_inline]] inline std::uint32_t id() noexcept { return READ_PCP(topology).get<"cpu-id">(); }
[[gnu::always_inline]] inline std::uint32_t numa_node() noexcept { return READ_PCP(topology).get<"numa-node">(); }
[[gnu::always_inline]] inline PerCpu *self() noexcept { return READ_PCP(self); }

// Used by QSpinlock
[[nodiscard, gnu::always_inline]] inline utils::CLHNode *get_curr_node() noexcept { return READ_PCP(curr_node); }
[[gnu::always_inline]] inline void set_curr_node(utils::CLHNode *node) noexcept { WRITE_PCP(curr_node, node); }
[[nodiscard, gnu::always_inline]] inline utils::CLHNode *get_prev_node() noexcept { return READ_PCP(prev_node); }
[[gnu::always_inline]] inline void set_prev_node(utils::CLHNode *node) noexcept { WRITE_PCP(prev_node, node); }

// CPU context
inline constexpr std::size_t CTX_BASE = offsetof(PerCpu, context);
inline constexpr std::size_t AST_BYTE = CTX_BASE + 0;
inline constexpr std::size_t PREEMPT_BYTE = CTX_BASE + 1;
inline constexpr std::size_t SOFTIRQ_BYTE = CTX_BASE + 2;
inline constexpr std::size_t HARDIRQ_BYTE = CTX_BASE + 3;
inline constexpr std::size_t NMI_MCE_BYTE = CTX_BASE + 4; // Nibble-split

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

[[gnu::always_inline]] inline void nmi_enter() noexcept { gs::add<NMI_MCE_BYTE, 1>(1); }
[[gnu::always_inline]] inline void nmi_exit() noexcept { gs::sub<NMI_MCE_BYTE, 1>(1); }

[[nodiscard, gnu::always_inline]] inline bool in_nmi() noexcept {
  return (gs::read<NMI_MCE_BYTE, std::uint8_t>() & 0x0F) > 0;
}

[[gnu::always_inline]] inline void mce_enter() noexcept {
  // Add 1 to the upper nibble (1 << 4 = 16).
  gs::add<NMI_MCE_BYTE, 1>(16);
}

[[gnu::always_inline]] inline void mce_exit() noexcept { gs::sub<NMI_MCE_BYTE, 1>(16); }

[[nodiscard, gnu::always_inline]] inline bool in_mce() noexcept {
  return (gs::read<NMI_MCE_BYTE, std::uint8_t>() & 0xF0) > 0;
}

[[nodiscard, gnu::always_inline]] inline bool in_interrupt() noexcept {
  // Reads Bytes 2 (soft), 3 (hard), 4 (nmi/mce), and 5 (unused zeros)
  const auto combined_irqs = gs::read<SOFTIRQ_BYTE, std::uint32_t>();
  return combined_irqs != 0;
}

[[nodiscard, gnu::always_inline]] inline bool can_sleep() noexcept {
  // Reads Preempt (Byte 1), Soft (Byte 2), Hard (Byte 3), NMI/MCE (Byte 4)
  const auto combined_state = gs::read<PREEMPT_BYTE, std::uint32_t>();
  return combined_state == 0;
}

void early_initialize() noexcept;
} // namespace percpu
} // namespace kernel::hw