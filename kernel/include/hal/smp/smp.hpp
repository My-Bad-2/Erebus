/// @file smp.hpp
/// @brief CPU limits, the CPU registry and application processor bring-up.
///
/// Every CPU the kernel runs on gets a dense ID: the BSP is 0 and the APs follow in the order the
/// boot protocol lists them. Per-CPU tables all over the kernel (page caches, slab caches, PCID
/// trackers, IPI inboxes) are plain arrays indexed by that ID, so hardware IDs, which can be
/// sparse and 32 bits wide, never leave this module and the APIC driver.
///
/// The registry records, per CPU, what other CPUs need to reach it: its per-CPU area, its APIC
/// IDs and its NUMA node. It also tracks the CPU's life cycle, which is what decides whether IPIs
/// may be sent to it.

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

#include "boot/boot.hpp"
#include "memory/address.hpp"

namespace kernel::hw {
struct PerCpu;
struct PerNumaNode;
}  // namespace kernel::hw

namespace kernel::hw::smp {
/// @brief Most logical CPUs the kernel will run on.
///
/// APIC IDs are 32 bits wide, but what actually matters is how many CPUs one machine can have:
/// the largest single-image systems today have a few thousand threads, and Linux stops at 8192.
/// 65536 is far beyond either while keeping a CPU number small enough to pack, which the
/// QSpinlock tail relies on. CPUs the firmware reports past this limit are never started.
inline constexpr std::uint32_t MAX_CPUS = 1u << 16;

/// @brief Most NUMA nodes the memory manager will track.
///
/// ACPI proximity domains are 32-bit as well. Nodes are numbered densely and some per-node
/// tables are quadratic in the node count, so the cap mostly protects against firmware that
/// invents domains. Real machines, CXL and HBM included, stay in the dozens.
inline constexpr std::uint32_t MAX_NUMA_NODES = 1u << 12;

/// @brief Returned by lookups that find no CPU.
inline constexpr std::uint32_t INVALID_CPU = ~0u;

/// @brief Where a CPU is in its life.
enum class CpuState : std::uint8_t {
  Offline,  ///< Not started yet.
  Booting,  ///< Started by the BSP and setting itself up.
  Online,   ///< Takes interrupts and IPIs.
  Failed,   ///< Did not come up in time. It is left alone, and parks itself if it ever wakes.
  Stopped,  ///< Parked for good by stop_other_cpus().
};

/// @brief Everything another CPU needs to know to talk to a CPU.
struct CpuEntry {
  PerCpu* area{nullptr};
  std::uint32_t apic_id{0};        ///< Physical (x2)APIC ID.
  std::uint32_t logical_dest{0};   ///< x2APIC logical ID, `cluster << 16 | bit`. 0 in xAPIC mode.
  std::uint32_t acpi_uid{0};       ///< ACPI processor UID, as found in the MADT.
  std::uint32_t numa_node{0};      ///< Dense node ID, as used by the PMM.
  std::uint32_t node_slot{0};      ///< Position among the CPUs of the node, see sched::NumaDomain.
  std::uint8_t nmi_lints{0};       ///< Bit n set: the MADT wires LINTn to NMI on this CPU.
  std::uint8_t nmi_active_low{0};  ///< Bit n set: that NMI line is active low.
  std::atomic<CpuState> state{CpuState::Offline};
};

namespace detail {
extern CpuEntry* g_cpus;
extern std::atomic<std::uint64_t>* g_online_mask;
extern std::uint32_t g_mask_words;
extern std::atomic<std::uint32_t> g_online_count;
extern PerNumaNode** g_nodes;
extern std::uint32_t g_node_count;
}  // namespace detail

/// @brief Number of CPUs the kernel will use, online or not.
///
/// Valid from the first instruction, since it only reads the boot protocol's answer. Per-CPU
/// tables should be sized by this.
[[nodiscard]] inline std::uint32_t possible_cpus() noexcept {
  const auto* resp = boot::mp_request.response;
  if (!resp || resp->cpu_count == 0) {
    return 1;
  }

  return resp->cpu_count < MAX_CPUS ? static_cast<std::uint32_t>(resp->cpu_count) : MAX_CPUS;
}

/// @brief Number of CPUs that are currently online. 1 until initialize() has run.
[[nodiscard]] inline std::uint32_t online_cpus() noexcept {
  const std::uint32_t count = detail::g_online_count.load(std::memory_order_acquire);
  return count != 0 ? count : 1;
}

/// @brief Registry entry of CPU @p cpu.
/// @pre `cpu < possible_cpus()` and initialize() has run.
[[nodiscard]] inline CpuEntry& entry(const std::uint32_t cpu) noexcept {
  return detail::g_cpus[cpu];
}

/// @brief Number of NUMA nodes. Their dense IDs are the ones the PMM uses.
[[nodiscard]] inline std::uint32_t node_count() noexcept { return detail::g_node_count; }

/// @brief The per-node area of NUMA node @p node, in memory of that node.
/// @pre `node < node_count()`.
[[nodiscard]] inline PerNumaNode* node_area(const std::uint32_t node) noexcept {
  return detail::g_nodes[node];
}

/// @brief Whether CPU @p cpu takes interrupts and IPIs right now.
[[nodiscard]] inline bool is_online(const std::uint32_t cpu) noexcept {
  if (cpu / 64 >= detail::g_mask_words) {
    return false;
  }

  return (detail::g_online_mask[cpu / 64].load(std::memory_order_acquire) >> (cpu % 64)) & 1;
}

/// @brief Calls `fn(cpu)` for every online CPU, in ascending ID order.
///
/// CPUs that come online during the walk may or may not be visited.
template <typename Fn>
void for_each_online(Fn&& fn) noexcept {
  for (std::uint32_t word = 0; word < detail::g_mask_words; ++word) {
    std::uint64_t bits = detail::g_online_mask[word].load(std::memory_order_acquire);

    while (bits != 0) {
      fn(word * 64 + static_cast<std::uint32_t>(std::countr_zero(bits)));
      bits &= bits - 1;
    }
  }
}

/// @brief Whether every CPU the firmware reported is online.
///
/// Only then can an IPI be broadcast with a destination shorthand: "all but self" also reaches
/// CPUs that are still parked in the boot loader or that never came up.
[[nodiscard]] bool all_online() noexcept;

/// @brief Whether IPIs go out through x2APIC logical (cluster) destinations.
[[nodiscard]] bool uses_cluster_destinations() noexcept;

/// @brief Gives the APs something to run once they are online, before they settle in the idle
///        loop: @p main runs on every AP, in thread context with interrupts enabled, and the AP
///        goes idle when it returns.
///
/// For code that wants the other CPUs at its disposal, such as the multi-CPU tests of the test
/// kernel. Takes effect for APs started after the call, so set it before initialize().
void set_ap_main(void (*main)() noexcept) noexcept;

/// @brief Brings up the BSP's interrupt hardware and starts every AP.
///
/// Returns once all APs are online, or once the ones that are not have timed out; those are
/// marked Failed and never used. Interrupts are enabled on the BSP when this returns.
/// @param bsp_stack_top Top of the stack the BSP runs on from now on.
void initialize(memory::VirtualAddress bsp_stack_top) noexcept;

/// @brief Stops every other CPU for good, for use by panics.
///
/// Sends an NMI to every online CPU, which parks it with interrupts disabled, and waits briefly
/// for them to confirm. Only the first CPU to get here does anything; a CPU that loses the race
/// parks itself instead, so two CPUs that panic at once cannot stop each other half way.
/// Before initialize() this does nothing, as nobody else is running.
void stop_other_cpus() noexcept;

/// @brief Called from the NMI handler. Parks this CPU if a stop is in progress.
/// @return false if the NMI has nothing to do with stopping CPUs.
bool handle_stop_nmi() noexcept;
}  // namespace kernel::hw::smp
