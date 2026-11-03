#include "memory/pmm/daemon.hpp"

#include "memory/pmm/router.hpp"

namespace kernel::memory::pmm {
namespace {
constexpr std::uint32_t COMPACTION_FRAG_THRESHOLD = 85;
constexpr std::array RECLAIMABLE_ZONES = {PageMobility::Unmovable, PageMobility::Movable,
                                          PageMobility::Reclaimable};

// Wake-ups are coalesced: a request only needs to reach the thread once, however many
// allocations cross the watermark before it gets to run.
struct alignas(std::hardware_destructive_interference_size) DaemonControlBlock {
  std::atomic<bool> reclaim_pending{false};
  std::atomic<bool> compact_pending{false};

  void* reclaim_thread{nullptr};
  void* compact_thread{nullptr};
};

// Allocated once the scheduler can create the daemon threads.
DaemonControlBlock* g_dcbs = nullptr;

// Body of the per-node reclaim thread, kept here so the policy lives next to the trigger. Not
// started yet: there is no scheduler to run it on.
[[maybe_unused]] void reclaim_worker_loop(const std::uint32_t node_id) noexcept {
  DaemonControlBlock& dcb = g_dcbs[node_id];
  NumaNode& node = g_router.get_node(node_id);

  while (true) {
    dcb.reclaim_pending.exchange(false, std::memory_order_acquire);

    bool pressure_resolved = true;

    for (const PageMobility mobility : RECLAIMABLE_ZONES) {
      MobilityZone& zone = node.get_zone(mobility);
      if (zone.get_free_pages() < zone.get_watermarks().high) {
        pressure_resolved = false;

        // TODO: Shrink the slab caches, swap out inactive pages, drop clean page cache.
      }
    }

    if (pressure_resolved) {
      // TODO: Block until the next wake_reclaim_daemon().
    } else {
      // Re-arm and yield instead of spinning while nothing can be reclaimed.
      dcb.reclaim_pending.store(true, std::memory_order_release);
      // TODO: Yield.
    }
  }
}

// Body of the per-node compaction thread. Same caveat as above.
[[maybe_unused]] void compaction_worker_loop(const std::uint32_t node_id) noexcept {
  DaemonControlBlock& dcb = g_dcbs[node_id];
  NumaNode& node = g_router.get_node(node_id);

  while (true) {
    dcb.compact_pending.exchange(false, std::memory_order_acquire);
    const MobilityZone& zone = node.get_zone(PageMobility::Movable);

    if (zone.calculate_fragmentation_index() > COMPACTION_FRAG_THRESHOLD) {
      // TODO: Pick a sparsely used 2 MiB block, migrate its movable pages elsewhere (copy,
      // update the page tables, shoot down the TLBs) and free the old frames.
      dcb.compact_pending.store(true, std::memory_order_release);
      // TODO: Yield.
    } else {
      // TODO: Block until the next wake_compaction_daemon().
    }
  }
}
}  // namespace

void wake_reclaim_daemon(const std::uint32_t node_id) noexcept {
  if (g_dcbs && !g_dcbs[node_id].reclaim_pending.exchange(true, std::memory_order_release)) {
    // TODO: Wake g_dcbs[node_id].reclaim_thread once threads exist.
  }
}

void wake_compaction_daemon(const std::uint32_t node_id) noexcept {
  if (g_dcbs && !g_dcbs[node_id].compact_pending.exchange(true, std::memory_order_release)) {
    // TODO: Wake g_dcbs[node_id].compact_thread once threads exist.
  }
}
}  // namespace kernel::memory::pmm
