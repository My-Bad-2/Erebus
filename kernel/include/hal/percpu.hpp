#pragma once

#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

#include "crypto/blake2b_prng.hpp"
#include "gdt.hpp"
#include "gs.hpp"
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

class CpuContext {
  std::uint64_t m_data;

public:
  constexpr explicit CpuContext(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, ast_flags, 0, 8)      // Asynchronous traps count
  BF_RW(std::uint8_t, preempt_count, 8, 8)  // Preemption depth
  BF_RW(std::uint8_t, softirq_count, 16, 8) // Deferred work
  BF_RW(std::uint8_t, hardirq_count, 24, 8) // IRQ nesting
  BF_RW(std::uint8_t, nmi_count, 32, 8)     // NMI nesting
  BF_RW(std::uint8_t, mce_count, 40, 8)     // Machine Check Exception nesting
};

struct alignas(std::hardware_destructive_interference_size) PerCpu {
  PerCpu *self{this};
  PerNumaNode *numa_node{nullptr};
  const CpuInfo *info{nullptr};
  CpuTopology topology{0};
  gdt::GlobalDescriptorTable gdt_table{};

  alignas(std::hardware_destructive_interference_size) CpuContext context{0};
  std::array<utils::CLHNode, 2> node;
  utils::CLHNode *curr_node{&node[0]};
  utils::CLHNode *prev_node{&node[1]};

  alignas(std::hardware_destructive_interference_size) memory::vmm::tlb::PcidManager pcid_manager;
  crypto::Blake2bPrng rng;

  constexpr explicit PerCpu() noexcept : rng(crypto::Blake2bPrng::create()) {}
};

namespace percpu {
[[gnu::always_inline]] inline std::uint32_t id() noexcept { return READ_PCP(topology).get_cpu_id(); }
[[gnu::always_inline]] inline std::uint32_t numa_node_id() noexcept { return READ_PCP(topology).get_numa_node(); }
[[gnu::always_inline]] inline PerCpu *self() noexcept { return READ_PCP(self); }
[[gnu::always_inline]] inline crypto::Blake2bPrng &rng() noexcept { return self()->rng; }
[[nodiscard]] inline memory::vmm::tlb::PcidManager &pcid_manager() noexcept { return self()->pcid_manager; }

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
  const auto combined_irqs = gs::read<SOFTIRQ_BYTE, std::uint64_t>();
  return combined_irqs != 0;
}

[[nodiscard, gnu::always_inline]] inline bool can_sleep() noexcept {
  // Reads Preempt (Byte 1), Soft (Byte 2), Hard (Byte 3), NMI/MCE (Byte 4)
  const auto combined_state = gs::read<PREEMPT_BYTE, std::uint64_t>();
  return combined_state == 0;
}

void early_initialize() noexcept;
} // namespace percpu
} // namespace kernel::hw