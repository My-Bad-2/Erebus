/// @file daemon.hpp
/// @brief Hooks into the per-node reclaim and compaction daemons.
///
/// The zones call these when they cross a watermark (see MobilityZone::evaluate_watermarks()).
/// The daemons need kernel threads, which do not exist yet, so for now a wake-up only records
/// that work is pending; `g_daemons_active` stays false and the zones do not even call in.

#pragma once

#include <cstdint>

namespace kernel::memory::pmm {
/// @brief Requests compaction of @p node_id (free memory exists but is too fragmented).
void wake_compaction_daemon(std::uint32_t node_id) noexcept;

/// @brief Requests reclaim on @p node_id (free memory dropped below the low watermark).
void wake_reclaim_daemon(std::uint32_t node_id) noexcept;
}  // namespace kernel::memory::pmm
