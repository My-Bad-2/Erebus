// What reclaim can get back without taking anything from anyone: empty slabs, cached pages and
// stacks, and the shrinker list that leads the daemons to all of it.
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/pmm/daemon.hpp"
#include "memory/shrinker.hpp"
#include "sched/reaper.hpp"
#include "sched/sleep.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using memory::PhysicalAddress;
using memory::Shrinker;
using memory::heap::KmemCache;
using memory::pmm::PageMobility;
using support::wait_for;

constexpr std::size_t OBJECTS = 4000;
void* g_objects[OBJECTS];

struct Counted {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<std::uint32_t> last_node{~0u};
  std::size_t pages{7};
};

std::size_t count_call(const std::uint32_t node_id, void* context) noexcept {
  auto* counted = static_cast<Counted*>(context);
  counted->last_node.store(node_id, std::memory_order_relaxed);
  counted->calls.fetch_add(1, std::memory_order_acq_rel);
  return counted->pages;
}

[[nodiscard]] std::uint32_t here() noexcept { return hw::percpu::numa_node_id(); }
}  // namespace

// A burst of objects leaves a trail of empty slabs once it is over. Some stay with the node for
// the next burst, which a shrink takes back; a second shrink finds nothing.
KTEST(Reclaim, a_shrink_takes_back_the_empty_slabs) {
  const auto cache = KmemCache::create(256, 8);
  KREQUIRE(cache.has_value());

  for (void*& object : g_objects) {
    const auto got = (*cache)->alloc();
    KREQUIRE(got.has_value());
    object = *got;
  }

  for (void* object : g_objects) {
    (*cache)->free(object);
  }

  KEXPECT_GT((*cache)->shrink_node(here()), 0u);
  KEXPECT_EQ((*cache)->shrink_node(here()), 0u);

  // And the cache works as before.
  const auto again = (*cache)->alloc();
  KREQUIRE(again.has_value());
  (*cache)->free(*again);

  KEXPECT_EQ((*cache)->shrink_node(memory::pmm::g_topology.active_nodes()), 0u);
  (*cache)->destroy();
}

// Slabs another CPU filled, and this one emptied, come back as well. Whatever that CPU still keeps
// for itself is its own to let go of, which is what the IPI of a shrink is for.
KTEST(Reclaim, a_shrink_gets_slabs_another_cpu_filled) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  const auto cache = KmemCache::create(512, 8);
  KREQUIRE(cache.has_value());

  struct Scene {
    KmemCache* cache;
    std::atomic<bool> done{false};
  } scene{.cache = *cache};

  // Allocated over there, freed here: every slab ends up frozen on the other CPU's partial list,
  // or empty on the node's.
  const std::uint32_t other = support::last_cpu();
  KREQUIRE(support::start(
               "alloc",
               +[](Scene* s) noexcept {
                 for (void*& object : g_objects) {
                   const auto got = s->cache->alloc();
                   object = got ? *got : nullptr;
                 }

                 s->done.store(true, std::memory_order_release);
               },
               &scene, support::pinned_to(other)) != nullptr);
  KEXPECT(wait_for([&] { return scene.done.load(std::memory_order_acquire); }));

  for (void* object : g_objects) {
    if (object != nullptr) {
      (*cache)->free(object);
    }
  }

  const std::uint32_t node = hw::smp::entry(other).numa_node;
  KEXPECT_GT((*cache)->shrink_node(node), 0u);
  (*cache)->destroy();
}

KTEST(Reclaim, the_whole_heap_shrinks_at_once) {
  for (std::size_t i = 0; i < OBJECTS; ++i) {
    g_objects[i] = memory::heap::kmalloc(2048);
    KREQUIRE(g_objects[i] != nullptr);
  }

  for (void* object : g_objects) {
    memory::heap::kfree(object);
  }

  KEXPECT_GT(memory::heap::shrink_node(here()), 0u);

  void* fresh = memory::heap::kmalloc(2048);
  KEXPECT(fresh != nullptr);
  memory::heap::kfree(fresh);

  KEXPECT_EQ(memory::heap::shrink_node(memory::pmm::g_topology.active_nodes()), 0u);
}

// Pages freed on a CPU wait in its cache, where the zone does not count them. A drain gives them
// back, from every CPU.
KTEST(Reclaim, a_drain_gives_cached_pages_back_to_the_zones) {
  constexpr std::size_t PAGES = 16;
  PhysicalAddress pages[PAGES];

  for (PhysicalAddress& page : pages) {
    const auto got = memory::pmm::alloc_pages(PageMobility::Movable, 0);
    KREQUIRE(got.has_value());
    page = *got;
  }

  for (const PhysicalAddress page : pages) {
    memory::pmm::free_pages(page, 0);
  }

  KEXPECT_GT(memory::pmm::drain_cpu_caches(here()), 0u);
  KEXPECT_EQ(memory::pmm::drain_cpu_caches(memory::pmm::g_topology.active_nodes()), 0u);
}

KTEST(Reclaim, shrinkers_are_asked_until_they_leave) {
  Counted counted;
  Shrinker shrinker{&count_call, &counted};

  memory::register_shrinker(shrinker);
  KEXPECT_GE(memory::run_shrinkers(0), counted.pages);
  KEXPECT_EQ(counted.calls.load(), 1u);
  KEXPECT_EQ(counted.last_node.load(), 0u);

  memory::unregister_shrinker(shrinker);
  (void)memory::run_shrinkers(0);
  KEXPECT_EQ(counted.calls.load(), 1u);
}

// A shrinker in the middle of a call stays on the list until the call is over, and whoever wants
// it gone waits for that.
KTEST(Reclaim, a_shrinker_is_not_taken_away_from_a_call) {
  struct Scene {
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};
    std::atomic<bool> gone{false};
    Shrinker* shrinker{nullptr};
  } scene;

  Shrinker shrinker{[](std::uint32_t, void* s) noexcept -> std::size_t {
                      auto* scene = static_cast<Scene*>(s);
                      if (scene->inside.exchange(true)) {
                        return 0;  // a daemon came along too
                      }

                      support::sleep_until_set(scene->release);
                      return 0;
                    },
                    &scene};
  scene.shrinker = &shrinker;
  memory::register_shrinker(shrinker);

  KREQUIRE(support::start(
               "shrink",
               +[](Scene* s) noexcept {
                 (void)memory::run_shrinkers(0);
                 s->ran.store(true, std::memory_order_release);
               },
               &scene) != nullptr);
  KEXPECT(wait_for([&] { return scene.inside.load(std::memory_order_acquire); }));

  KREQUIRE(support::start(
               "leave",
               +[](Scene* s) noexcept {
                 memory::unregister_shrinker(*s->shrinker);
                 s->gone.store(true, std::memory_order_release);
               },
               &scene) != nullptr);

  (void)sched::sleep_for(Duration::milliseconds(20));
  KEXPECT(!scene.gone.load(std::memory_order_acquire));

  scene.release.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] {
    return scene.ran.load(std::memory_order_acquire) && scene.gone.load(std::memory_order_acquire);
  }));
}

// Thread stacks are among what reclaim takes back.
KTEST(Reclaim, cached_stacks_go_back_under_pressure) {
  // Two threads, so that at least one stack is cached when they are gone.
  struct Done {
    std::atomic<std::uint32_t> count{0};
  } done;

  const std::uint64_t reaped = sched::reaper::reaped_count();
  for (int i = 0; i < 2; ++i) {
    KREQUIRE(support::start(
                 "stack", +[](Done* d) noexcept { d->count.fetch_add(1); }, &done) != nullptr);
  }

  KEXPECT(wait_for([&] { return done.count.load() == 2; }));
  KEXPECT(wait_for([&] { return sched::reaper::reaped_count() >= reaped + 2; }));
  KEXPECT_GT(sched::Thread::stack_cache_stats().cached, 0u);

  for (std::uint32_t node = 0; node < memory::pmm::g_topology.active_nodes(); ++node) {
    (void)memory::run_shrinkers(node);
  }

  KEXPECT_EQ(sched::Thread::stack_cache_stats().cached, 0u);
}

// Waking a daemon for nothing costs a look, not a pass, and certainly not a CPU.
KTEST(Reclaim, a_daemon_with_nothing_to_do_stays_quiet) {
  const memory::pmm::DaemonStats before = memory::pmm::daemon_stats(0);

  memory::pmm::wake_reclaim_daemon(0);
  memory::pmm::wake_compaction_daemon(0);
  (void)sched::sleep_for(Duration::milliseconds(50));
  const memory::pmm::DaemonStats woken = memory::pmm::daemon_stats(0);

  (void)sched::sleep_for(Duration::milliseconds(200));
  const memory::pmm::DaemonStats later = memory::pmm::daemon_stats(0);

  // Compaction may well have something to try, as fragmentation is measured in 1 GiB blocks. What
  // it must not do is keep at it.
  KEXPECT_EQ(later.compaction_passes, woken.compaction_passes);
  KEXPECT_EQ(later.reclaim_passes, woken.reclaim_passes);
  KEXPECT_GE(woken.reclaim_passes, before.reclaim_passes);
}

// Taking a zone below its low watermark wakes reclaim, which does a pass. Once the memory is back,
// the daemon goes to sleep again.
KTEST(Reclaim, pressure_wakes_reclaim_and_relief_quiets_it) {
  memory::pmm::MobilityZone& zone =
      memory::pmm::g_router.get_node(here()).get_zone(PageMobility::Movable);
  const std::uint64_t low = zone.get_watermarks().low;
  if (low == 0 || zone.get_free_pages() <= low) {
    KTEST_SKIP("the zone is too small, or below its watermark already");
  }

  const memory::pmm::DaemonStats before = memory::pmm::daemon_stats(here());

  // Chained through their own first word, so that this needs no memory of its own.
  PhysicalAddress chain{};
  std::size_t taken = 0;
  while (zone.get_free_pages() > low / 2) {
    const auto page = memory::pmm::alloc_pages_on_node(here(), PageMobility::Movable, 0);
    if (!page) {
      break;
    }

    *memory::DirectMap::phys_to_virt(*page).as<std::uint64_t>() = chain.value();
    chain = *page;
    ++taken;
  }

  KEXPECT_GT(taken, 0u);
  KEXPECT(wait_for(
      [&] { return memory::pmm::daemon_stats(here()).reclaim_passes > before.reclaim_passes; }));

  // Nothing it can give back stays taken. It may find a few things at first, then it has to back
  // off, not go round and round: a pass every few hundred milliseconds at most, nowhere near the
  // thousands a loop would make.
  (void)sched::sleep_for(Duration::milliseconds(200));
  const std::uint64_t held = memory::pmm::daemon_stats(here()).reclaim_passes;
  (void)sched::sleep_for(Duration::milliseconds(400));
  KEXPECT_LE(memory::pmm::daemon_stats(here()).reclaim_passes - held, 10u);

  while (chain.value() != 0) {
    const PhysicalAddress next{*memory::DirectMap::phys_to_virt(chain).as<std::uint64_t>()};
    memory::pmm::free_pages(chain, 0);
    chain = next;
  }

  // A pass that was under way when the pages came back may still finish, and one more look may
  // follow the pause after it. After that, nothing.
  (void)sched::sleep_for(Duration::milliseconds(1500));
  const std::uint64_t settled = memory::pmm::daemon_stats(here()).reclaim_passes;
  (void)sched::sleep_for(Duration::milliseconds(300));
  KEXPECT_EQ(memory::pmm::daemon_stats(here()).reclaim_passes, settled);
}
}  // namespace kernel::testing
