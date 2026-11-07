/// @file balance.hpp
/// @brief Which CPU a thread should be on, as far as the machine is concerned.
///
/// How much it costs to move a thread depends on what the two CPUs share. Between SMT siblings
/// nothing is lost. Within a cluster, the L1 goes. Within a shared last-level cache, the L2 does
/// too, but the data is a few nanoseconds away. Beyond that the thread starts over, and to another
/// node, it leaves its memory behind for good. So a thread that wakes up looks for an idle CPU
/// around where it last ran, and a CPU that has nothing to do looks for work around itself.
///
/// ### Nested groups
///
/// There are no domain structures to walk. Inside a node, CPUs get slots in the order of their
/// APIC IDs, whose bits are laid out from the package down to the SMT thread. That leaves all the
/// CPUs of any core, cluster, cache or package next to each other, so a group is just a range
/// of slots, and looking in it is a masked look at a few words of the node's bitmaps (see
/// sched::NumaDomain). What is close by gets looked at first simply by making the range wider.
///
/// ### Not all CPUs are the same
///
/// On a hybrid machine, CPUs differ in what they get done. A CPU's *capacity* says how much, from
/// FULL_CAPACITY for the fastest in the machine on down. It decides which of several idle CPUs
/// gets a thread, and evens out load comparisons: three threads on a CPU of half the capacity are
/// worse off than five on a full one.
///
/// All of this only says where threads should go. Nothing here ever makes a running thread move.
#pragma once

#include <cstdint>
#include <span>

#include "hal/smp/cpu_info.hpp"

namespace kernel::sched {
class RunQueue;
class Thread;

/// @brief What the fastest CPUs of the machine have.
inline constexpr std::uint32_t FULL_CAPACITY = 1024;

/// @brief How a small core compares to a big one at the same clock, in percent.
///
/// CPUID has nothing to say about this, and it varies from one workload to the next. This is a
/// round figure rather than a measurement. All that it has to get right is the order.
inline constexpr std::uint32_t SMALL_CORE_PERCENT = 60;

/// @brief The clock to assume for a CPU that does not say. All that counts is that it is the same
///        for all of them, and on the machines that do not say, it is.
inline constexpr std::uint32_t UNKNOWN_MHZ = 1000;

/// @brief What a CPU can get done, in units that only mean something compared to another CPU.
/// @param type    What kind of core it is.
/// @param max_mhz Its top clock, or 0 if it does not say.
[[nodiscard]] constexpr std::uint32_t core_performance(hw::CoreType type,
                                                       std::uint32_t max_mhz) noexcept {
  const std::uint32_t mhz = max_mhz != 0 ? max_mhz : UNKNOWN_MHZ;
  return mhz * (type == hw::CoreType::Atom ? SMALL_CORE_PERCENT : 100);
}

/// @brief The capacity of a CPU, given what the fastest one in the machine can do.
[[nodiscard]] constexpr std::uint32_t scale_capacity(const std::uint32_t performance,
                                                     const std::uint32_t fastest) noexcept {
  if (fastest == 0 || performance >= fastest) {
    return FULL_CAPACITY;
  }

  const std::uint64_t scaled = std::uint64_t{performance} * FULL_CAPACITY / fastest;
  return scaled != 0 ? static_cast<std::uint32_t>(scaled) : 1;
}

/// @brief Whether a thread does better on CPU A than on CPU B, given their loads and capacities.
///
/// What a thread gets is capacity / threads, counting itself as it would be there. Multiplied out
/// to do without a division, which also leaves no rounding to break a tie.
[[nodiscard]] constexpr bool is_less_busy(std::uint32_t load_a, std::uint32_t capacity_a,
                                          std::uint32_t load_b, std::uint32_t capacity_b) noexcept {
  return std::uint64_t{load_a + 1} * capacity_b < std::uint64_t{load_b + 1} * capacity_a;
}

/// @brief How many low APIC ID bits tell CPUs apart within each kind of group.
///
/// Shifting an APIC ID right by one of these gives the ID of the group. Each one is at least as
/// large as the one before.
struct SharingShifts {
  std::uint32_t core{0};     ///< Within a core: SMT threads.
  std::uint32_t cluster{0};  ///< Within what shares a mid-level cache, or a module.
  std::uint32_t cache{0};    ///< Within what shares the last-level cache.
  std::uint32_t package{0};  ///< Within a package.
};

/// @brief Works those out from what a CPU says about itself.
///
/// What it does not say is taken to be the widest it can be. With no idea of the caches, that makes
/// a package one big cache, which is a safe thing to be wrong about.
/// @param levels What it says about its topology, innermost first.
/// @param caches What it says about its caches.
[[nodiscard]] SharingShifts sharing_shifts(std::span<const hw::TopologyInfo> levels,
                                           std::span<const hw::CacheInfo> caches) noexcept;

/// @brief sharing_shifts(), with what @p info has on the subject.
[[nodiscard]] inline SharingShifts sharing_shifts(const hw::CpuInfo& info) noexcept {
  return sharing_shifts(info.topology(), info.caches());
}

namespace balance {
/// @brief Builds every node's domain from the CPU registry.
void build_domains() noexcept;

/// @brief Works out how the CPUs compare, now that all of them are here to tell.
/// @pre Every CPU that is coming up has, and none of them went through this yet.
void publish_capacities() noexcept;

/// @brief The best CPU for @p thread right now. Claims it if it is idle, so the thread had better
///        go there. See Scheduler::select_target_cpu().
[[nodiscard]] std::uint32_t select_cpu(const Thread& thread) noexcept;

/// @brief Wakes an idle CPU to come and take a thread that is waiting on @p rq, if that helps.
void offer_spare_work(RunQueue& rq) noexcept;

/// @brief Moves a thread from a CPU that has one to spare to @p rq, nearest first.
/// @return Whether it got one.
[[nodiscard]] bool steal_work(RunQueue& rq) noexcept;
}  // namespace balance
}  // namespace kernel::sched
