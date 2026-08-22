#pragma once

#include <cstdint>

namespace kernel::memory::pmm {
void wake_compaction_daemon(std::uint32_t node_id) noexcept;
void wake_reclaim_daemon(std::uint32_t node_id) noexcept;
} // namespace kernel::memory::pmm