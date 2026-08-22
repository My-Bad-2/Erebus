#include "memory/pmm/daemon.hpp"
#include "memory/pmm/router.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
namespace {
struct alignas(64) DaemonControlBlock {
  std::atomic_flag reclaim_pending{ATOMIC_FLAG_INIT};
  std::atomic_flag compact_pending{ATOMIC_FLAG_INIT};

  void *reclaim_thread{nullptr};
  void *compact_thread{nullptr};
};

DaemonControlBlock *dcbs = nullptr;

void reclaim_worker_loop(std::uint32_t node_id) noexcept {
  DaemonControlBlock &dcb = dcbs[node_id];
  NumaNode &node = g_router.get_node(node_id);

  while (true) {
    dcb.reclaim_pending.clear(std::memory_order_acquire);

    bool memory_pressure_resolved = true;
    for (int i = 0; i < 3; ++i) {
      MobilityZone &zone = node.get_zone(static_cast<PageMobility>(i));

      if (zone.get_free_pages() < zone.get_watermarks().high) {
        memory_pressure_resolved = false;

        // reclaim pages via vmm
      }
    }

    if (memory_pressure_resolved) {
      // sleep until next trigger
    } else {
      dcb.reclaim_pending.test_and_set(std::memory_order_release);
      // yield
    }
  }
}

void compaction_worker_loop(std::uint32_t node_id) noexcept {
  DaemonControlBlock &dcb = dcbs[node_id];
  NumaNode &node = g_router.get_node(node_id);

  while (true) {
    dcb.compact_pending.clear(std::memory_order_acquire);
    MobilityZone &zone = node.get_zone(PageMobility::Movable);

    if (zone.calculate_fragmentation_index() > 85) {
      // PLAN:
      // isolate a 2mb phys block which have few 4kb pages in use
      // allocate fresh 4 kb page elsewhere
      // copy the data, update the VMM page tables, flush the TLB.
      // free the old 4kb

      dcb.compact_pending.test_and_set(std::memory_order_release);
      // yield
    } else {
      // sleep until next trigger
    }
  }
}
} // namespace

void wake_reclaim_daemon(const std::uint32_t node_id) noexcept {
  // if (!dcbs[node_id].reclaim_pending.test_and_set(std::memory_order_release)) {
  //   // wakeup reclaim thread
  // }

  utils::logger::fatal("Reclaim Daemon not implemented yet!\n");
}

void wake_compaction_daemon(const std::uint32_t node_id) noexcept {
  // if (!dcbs[node_id].compact_pending.test_and_set(std::memory_order_release)) {
  //   // wakeup compact thread
  // }

  utils::logger::fatal("Compaction Daemon not implemented yet!\n");
}
} // namespace kernel::memory::pmm