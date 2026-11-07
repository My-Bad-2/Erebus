#include "memory/pmm/daemon.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono.hpp>
#include <kformat.hpp>
#include <memory>
#include <new>

#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "memory/pmm/router.hpp"
#include "memory/shrinker.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"
#include "sched/thread.hpp"
#include "sched/wait_queue.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
namespace {
constexpr std::uint32_t COMPACTION_FRAG_THRESHOLD = 85;
constexpr std::array RECLAIMABLE_ZONES = {
    PageMobility::Unmovable,
    PageMobility::Movable,
    PageMobility::Reclaimable,
};

constexpr klib::Duration MIN_BACKOFF = klib::Duration::milliseconds(10);
constexpr klib::Duration MAX_BACKOFF = klib::Duration::seconds(1);

struct alignas(std::hardware_destructive_interference_size) DaemonControlBlock {
  std::atomic<bool> reclaim_pending{false};
  std::atomic<bool> compact_pending{false};
  sched::WaitQueue reclaim_wq{};
  sched::WaitQueue compact_wq{};
  sched::Thread* reclaim_thread{nullptr};
  sched::Thread* compact_thread{nullptr};

  std::uint32_t node_id{0};
  std::atomic<std::uint64_t> reclaim_passes{0};
  std::atomic<std::uint64_t> pages_reclaimed{0};
  std::atomic<std::uint64_t> compaction_passes{0};
};

std::atomic<DaemonControlBlock*> g_dcbs{nullptr};
std::uint32_t g_dcb_count = 0;

[[nodiscard]] bool below_high_watermark(NumaNode& node) noexcept {
  return std::ranges::any_of(RECLAIMABLE_ZONES, [&](const PageMobility mobility) {
    const MobilityZone& zone = node.get_zone(mobility);
    return zone.get_free_pages() < zone.get_watermarks().high;
  });
}

[[nodiscard]] klib::Duration longer(const klib::Duration backoff) noexcept {
  return std::min(backoff * 2, MAX_BACKOFF);
}

void reclaim_worker_loop(DaemonControlBlock* block) noexcept {
  DaemonControlBlock& dcb = *block;
  NumaNode& node = g_router.get_node(dcb.node_id);
  klib::Duration backoff = MIN_BACKOFF;

  while (true) {
    dcb.reclaim_pending.exchange(false, std::memory_order_acquire);

    bool pressure_resolved = !below_high_watermark(node);
    std::size_t given_back = 0;

    if (!pressure_resolved) {
      dcb.reclaim_passes.fetch_add(1, std::memory_order_relaxed);

      // Free already, only out of the zones' sight. That may be all it takes.
      const std::size_t drained = drain_cpu_caches(dcb.node_id);
      pressure_resolved = !below_high_watermark(node);

      if (!pressure_resolved) {
        given_back = run_shrinkers(dcb.node_id);
        pressure_resolved = !below_high_watermark(node);
      }

      dcb.pages_reclaimed.fetch_add(drained + given_back, std::memory_order_relaxed);
    }

    if (pressure_resolved) {
      backoff = MIN_BACKOFF;
      dcb.reclaim_wq.wait([&] { return dcb.reclaim_pending.load(std::memory_order_acquire); });
    } else if (given_back > 0) {
      // There was something to get, so there may be more. Others go first, then the next pass.
      backoff = MIN_BACKOFF;
      dcb.reclaim_pending.store(true, std::memory_order_release);
      sched::Scheduler::yield();
    } else {
      // Nothing came back: the node is full of memory that is in use. Asking again right away
      // gets the same answer, so the next pass waits for both a pause and a new request.
      (void)sched::sleep_for(backoff);
      backoff = longer(backoff);
      dcb.reclaim_wq.wait([&] { return dcb.reclaim_pending.load(std::memory_order_acquire); });
    }
  }
}

void compaction_worker_loop(DaemonControlBlock* block) noexcept {
  DaemonControlBlock& dcb = *block;
  NumaNode& node = g_router.get_node(dcb.node_id);

  while (true) {
    dcb.compact_pending.exchange(false, std::memory_order_acquire);
    const MobilityZone& zone = node.get_zone(PageMobility::Movable);

    const std::uint32_t before = zone.calculate_fragmentation_index();
    if (before > COMPACTION_FRAG_THRESHOLD) {
      dcb.compaction_passes.fetch_add(1, std::memory_order_relaxed);

      // What the CPUs keep is what stands between a lot of free blocks and their buddies. Moving
      // pages that are in use would be the next step, and needs reverse mappings (see daemon.hpp).
      dcb.pages_reclaimed.fetch_add(drain_cpu_caches(dcb.node_id), std::memory_order_relaxed);

      const std::uint32_t after = zone.calculate_fragmentation_index();
      if (after > COMPACTION_FRAG_THRESHOLD && after < before) {
        // Better, if not good enough yet. Others go first, then another look.
        dcb.compact_pending.store(true, std::memory_order_release);
        sched::Scheduler::yield();
        continue;
      }
    }

    // Either nothing to do, or nothing more this daemon knows how to do until memory changes.
    dcb.compact_wq.wait([&] { return dcb.compact_pending.load(std::memory_order_acquire); });
  }
}

[[nodiscard]] sched::Thread* start_daemon(const char* prefix,
                                          void (*body)(DaemonControlBlock*) noexcept,
                                          DaemonControlBlock& dcb) noexcept {
  std::array<char, sched::Thread::MAX_NAME_LEN> name{};
  (void)klib::kprint(name.data(), name.size(), "{}/{}", prefix, dcb.node_id);

  const sched::ThreadCreateOptions options{
      .priority = sched::PriorityClass::Background,
      .preferred_numa_node = dcb.node_id,
  };

  const auto thread = sched::Thread::spawn_kernel(name.data(), body, &dcb, options);
  if (!thread || !sched::Scheduler::enqueue(*thread)) {
    utils::logger::fatal("pmm: cannot start {}/{}\n", prefix, dcb.node_id);
  }

  return *thread;
}

[[nodiscard]] DaemonControlBlock* block_of(const std::uint32_t node_id) noexcept {
  DaemonControlBlock* dcbs = g_dcbs.load(std::memory_order_acquire);
  return dcbs != nullptr && node_id < g_dcb_count ? &dcbs[node_id] : nullptr;
}
}  // namespace

void initialize_daemons() noexcept {
  const std::uint32_t nodes = g_router.get_active_nodes();
  auto* dcbs = static_cast<DaemonControlBlock*>(
      heap::kmalloc(sizeof(DaemonControlBlock) * nodes, alignof(DaemonControlBlock)));
  if (dcbs == nullptr) {
    utils::logger::fatal("pmm: no memory for the daemons of {} nodes\n", nodes);
  }

  for (std::uint32_t node = 0; node < nodes; ++node) {
    std::construct_at(&dcbs[node])->node_id = node;
  }

  // The blocks go first, then the threads: a wake that comes in between only leaves the flag up,
  // and the first thing a daemon does is look at it.
  g_dcb_count = nodes;
  g_dcbs.store(dcbs, std::memory_order_release);

  for (std::uint32_t node = 0; node < nodes; ++node) {
    dcbs[node].reclaim_thread = start_daemon("kreclaimd", &reclaim_worker_loop, dcbs[node]);
    dcbs[node].compact_thread = start_daemon("kcompactd", &compaction_worker_loop, dcbs[node]);
  }

  g_daemons_active.store(true, std::memory_order_release);
}

void wake_reclaim_daemon(const std::uint32_t node_id) noexcept {
  DaemonControlBlock* dcb = block_of(node_id);
  if (dcb != nullptr && !dcb->reclaim_pending.exchange(true, std::memory_order_release)) {
    (void)dcb->reclaim_wq.wake_one();
  }
}

void wake_compaction_daemon(const std::uint32_t node_id) noexcept {
  DaemonControlBlock* dcb = block_of(node_id);
  if (dcb != nullptr && !dcb->compact_pending.exchange(true, std::memory_order_release)) {
    (void)dcb->compact_wq.wake_one();
  }
}

DaemonStats daemon_stats(const std::uint32_t node_id) noexcept {
  const DaemonControlBlock* dcb = block_of(node_id);
  if (dcb == nullptr) {
    return {};
  }

  return {
      .reclaim_passes = dcb->reclaim_passes.load(std::memory_order_relaxed),
      .pages_reclaimed = dcb->pages_reclaimed.load(std::memory_order_relaxed),
      .compaction_passes = dcb->compaction_passes.load(std::memory_order_relaxed),
  };
}
}  // namespace kernel::memory::pmm
