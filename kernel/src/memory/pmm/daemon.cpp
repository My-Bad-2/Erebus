#include "memory/pmm/daemon.hpp"
#include "memory/pmm/router.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
namespace {
constexpr std::uint32_t COMPACTION_FRAG_THRESHOLD = 85;
constexpr std::array RECLAIMABLE_ZONES = {PageMobility::Unmovable, PageMobility::Movable, PageMobility::Reclaimable};

struct alignas(64) DaemonControlBlock {
  std::atomic<bool> reclaim_pending{false};
  std::atomic<bool> compact_pending{false};

  void *reclaim_thread{nullptr};
  void *compact_thread{nullptr};
};

DaemonControlBlock *g_dcbs = nullptr;

void reclaim_worker_loop(const std::uint32_t node_id) noexcept {
  DaemonControlBlock &dcb = g_dcbs[node_id];
  NumaNode &node = g_router.get_node(node_id);

  while (true) {
    dcb.reclaim_pending.exchange(false, std::memory_order_acquire);

    bool pressure_resolved = true;

    for (const PageMobility mobility : RECLAIMABLE_ZONES) {
      MobilityZone &zone = node.get_zone(mobility);
      const auto [min, low, high] = zone.get_watermarks();

      if (zone.get_free_pages() < high) {
        pressure_resolved = false;

        // TODO: Reclaim pages via VMM
        // e.g., shrink SLUB caches, swap out inactive pages, drop pagecache
      }
    }

    if (pressure_resolved) {
      // TODO: Sleep/block thread until next trigger
    } else {
      // Re-arm pending flag and yield to prevent spinning 100% CPU on OOM
      dcb.reclaim_pending.store(true, std::memory_order_release);
      // TODO: yield
    }
  }
}

void compaction_worker_loop(const std::uint32_t node_id) noexcept {
  DaemonControlBlock &dcb = g_dcbs[node_id];
  NumaNode &node = g_router.get_node(node_id);

  while (true) {
    dcb.compact_pending.exchange(false, std::memory_order_acquire);
    MobilityZone &zone = node.get_zone(PageMobility::Movable);

    if (zone.calculate_fragmentation_index() > COMPACTION_FRAG_THRESHOLD) {
      // Isolate a 2MB phys block which has few 4KB pages in use.
      // Allocate fresh 4KB pages elsewhere.
      // Copy the data, update the VMM page tables, flush the TLB.
      // Free the old 4KB pages back to the zone.

      dcb.compact_pending.store(true, std::memory_order_release);
      // TODO: yield
    } else {
      // TODO: Sleep/block thread until next trigger
    }
  }
}
} // namespace

void wake_reclaim_daemon(const std::uint32_t node_id) noexcept {
  // if (g_dcbs && !g_dcbs[node_id].reclaim_pending.exchange(true, std::memory_order_release)) {
  //   // TODO: Wake up thread via scheduler
  // }

  utils::logger::fatal("Reclaim Daemon not implemented yet!\n");
}

void wake_compaction_daemon(const std::uint32_t node_id) noexcept {
  // if (g_dcbs && !g_dcbs[node_id].compact_pending.exchange(true, std::memory_order_release)) {
  //   // TODO: Wake up thread via scheduler
  // }

  utils::logger::fatal("Compaction Daemon not implemented yet!\n");
}
} // namespace kernel::memory::pmm