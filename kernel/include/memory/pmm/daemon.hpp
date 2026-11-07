/// @file daemon.hpp
/// @brief The per-node reclaim and compaction daemons, and the hooks that wake them.
///
/// Every node gets two kernel threads, `kreclaimd/N` and `kcompactd/N`, both in the Background
/// class and kept near the node. The zones call the wake functions when they cross a watermark
/// (see MobilityZone::evaluate_watermarks()), which is often under a zone lock and with
/// interrupts off: a wake costs one exchange when the daemon is already on its way, and that is
/// all most of them ever cost.
///
/// Reclaim takes back what the kernel keeps without needing it: the pages the CPUs cache in front
/// of the zones first, as that is cheap, then whatever the shrinkers (memory/shrinker.hpp) can
/// spare, until every zone of the node is above its high watermark again.
///
/// Compaction can only do half its job so far. Draining the CPU caches lets free blocks merge
/// with their buddies again, which is the half that works. Moving pages that are in use to make
/// room needs to know every mapping of a frame, and the VMM does not keep track of that yet.
///
/// Neither daemon spins. A pass that did not help is followed by a pause that grows each time,
/// and then by waiting for the next wake, so a node that is simply full costs a pass every so
/// often, and nothing in between.

#pragma once

#include <cstdint>

namespace kernel::memory::pmm {
/// @brief What a node's daemons have done so far.
struct DaemonStats {
  std::uint64_t reclaim_passes;
  std::uint64_t pages_reclaimed;  ///< From the CPU caches and the shrinkers together.
  std::uint64_t compaction_passes;
};

/// @brief Starts the daemons of every node and has the zones wake them from now on.
/// @pre The scheduler runs on every CPU and the heap works. Called once.
void initialize_daemons() noexcept;

/// @brief Requests compaction of @p node_id (free memory exists but is too fragmented).
void wake_compaction_daemon(std::uint32_t node_id) noexcept;

/// @brief Requests reclaim on @p node_id (free memory dropped below the low watermark).
void wake_reclaim_daemon(std::uint32_t node_id) noexcept;

/// @brief The work of @p node_id's daemons, all zero before initialize_daemons().
[[nodiscard]] DaemonStats daemon_stats(std::uint32_t node_id) noexcept;
}  // namespace kernel::memory::pmm
