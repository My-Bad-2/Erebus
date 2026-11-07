// Which CPUs are close to which. Everything that depends on the machine is on made-up tables here,
// so that a laptop can test what a two-socket server with big and small cores would do, and the
// last few tests check that what the machine this runs on says adds up.
#include "sched/balance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "sched/numa_domain.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
namespace smp = hw::smp;
using hw::CacheInfo;
using hw::CacheType;
using hw::CoreType;
using hw::TopoLevel;
using hw::TopologyInfo;
using sched::CpuPlacement;
using sched::FULL_CAPACITY;
using sched::NumaDomain;
using sched::Sharing;
using sched::SharingShifts;
using sched::SlotSpan;

constexpr std::uint32_t NO_CPU = NumaDomain::NO_CPU;

// Two packages of two caches of two clusters of two cores of two threads: 32 CPUs, numbered like
// their slots plus 100, so that it is easy to tell by the number where a CPU is.
constexpr std::uint32_t CPUS = 32;

[[nodiscard]] constexpr std::uint32_t cpu_in_slot(const std::uint32_t slot) noexcept {
  return 100 + slot;
}

class Machine {
  NumaDomain m_domain;

 public:
  Machine() noexcept {
    std::array<CpuPlacement, CPUS> cpus{};

    for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
      cpus[slot] = {.cpu = cpu_in_slot(slot),
                    .core = slot / 2,
                    .cluster = slot / 4,
                    .cache = slot / 8,
                    .package = slot / 16};
    }

    (void)m_domain.initialize(0, cpus, {});
  }

  ~Machine() noexcept { m_domain.release(); }

  [[nodiscard]] NumaDomain* operator->() noexcept { return &m_domain; }
  [[nodiscard]] NumaDomain& domain() noexcept { return m_domain; }

  void all_idle() noexcept {
    for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
      m_domain.mark_idle(slot);
    }
  }

  void all_busy() noexcept {
    for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
      m_domain.mark_busy(slot);
    }
  }
};

// The order for_each_overloaded_near() goes through the CPUs in.
struct Visits {
  std::array<std::uint32_t, CPUS> cpus{};
  std::uint32_t count{0};
};

[[nodiscard]] Visits visits_from(NumaDomain& domain, const std::uint32_t slot) noexcept {
  Visits visits;

  domain.for_each_overloaded_near(slot, [&](const std::uint32_t cpu) {
    if (visits.count < CPUS) {
      visits.cpus[visits.count] = cpu;
    }

    ++visits.count;
    return false;
  });

  return visits;
}
}  // namespace

static_assert(sched::core_performance(CoreType::Core, 3000) >
              sched::core_performance(CoreType::Atom, 3000));
static_assert(sched::core_performance(CoreType::Unknown, 2000) ==
              sched::core_performance(CoreType::Core, 2000));
static_assert(sched::core_performance(CoreType::Core, 0) ==
              sched::core_performance(CoreType::Unknown, 0));

// A faster clock can make up for a smaller core.
static_assert(sched::core_performance(CoreType::Atom, 4000) >
              sched::core_performance(CoreType::Core, 2000));

static_assert(sched::scale_capacity(100, 100) == FULL_CAPACITY);
static_assert(sched::scale_capacity(50, 100) == FULL_CAPACITY / 2);
static_assert(sched::scale_capacity(1, 1'000'000) == 1);
static_assert(sched::scale_capacity(100, 0) == FULL_CAPACITY);

static_assert(SlotSpan{4, 4}.contains(4) && SlotSpan{4, 4}.contains(7));
static_assert(!SlotSpan{4, 4}.contains(3) && !SlotSpan{4, 4}.contains(8));

// Three on a full CPU come to the same as one on half of one, given who gets the next thread:
// four against two, on capacities two to one.
static_assert(!sched::is_less_busy(1, FULL_CAPACITY / 2, 3, FULL_CAPACITY));
static_assert(sched::is_less_busy(0, FULL_CAPACITY / 2, 3, FULL_CAPACITY));
static_assert(sched::is_less_busy(3, FULL_CAPACITY, 2, FULL_CAPACITY / 2));
static_assert(!sched::is_less_busy(2, FULL_CAPACITY, 2, FULL_CAPACITY));

KTEST(Topology, shifts_of_a_simple_machine) {
  const std::array levels{
      TopologyInfo{.level_type = TopoLevel::SMT, .logical_processors = 2, .id_shift = 1},
      TopologyInfo{.level_type = TopoLevel::Core, .logical_processors = 16, .id_shift = 5},
  };
  const std::array caches{
      CacheInfo{.level = 1, .type = CacheType::Data, .threads_sharing = 2},
      CacheInfo{.level = 1, .type = CacheType::Instruction, .threads_sharing = 2},
      CacheInfo{.level = 2, .type = CacheType::Unified, .threads_sharing = 2},
      CacheInfo{.level = 3, .type = CacheType::Unified, .threads_sharing = 16},
  };

  const SharingShifts shifts = sched::sharing_shifts(levels, caches);

  KEXPECT_EQ(shifts.core, 1u);
  KEXPECT_EQ(shifts.cluster, 1u);
  KEXPECT_EQ(shifts.cache, 4u);
  KEXPECT_EQ(shifts.package, 5u);
}

// An AMD part with several caches to a package, and Intel small cores sharing an L2.
KTEST(Topology, a_cache_can_be_shared_by_less_than_a_package) {
  const std::array levels{
      TopologyInfo{.level_type = TopoLevel::SMT, .logical_processors = 2, .id_shift = 1},
      TopologyInfo{.level_type = TopoLevel::Core, .logical_processors = 32, .id_shift = 6},
  };
  const std::array caches{
      CacheInfo{.level = 2, .type = CacheType::Unified, .threads_sharing = 8},
      CacheInfo{.level = 3, .type = CacheType::Unified, .threads_sharing = 16},
  };

  const SharingShifts shifts = sched::sharing_shifts(levels, caches);

  KEXPECT_EQ(shifts.cluster, 3u);
  KEXPECT_EQ(shifts.cache, 4u);
  KEXPECT_EQ(shifts.package, 6u);
}

KTEST(Topology, a_module_counts_as_a_cluster) {
  const std::array levels{
      TopologyInfo{.level_type = TopoLevel::SMT, .logical_processors = 2, .id_shift = 1},
      TopologyInfo{.level_type = TopoLevel::Module, .logical_processors = 8, .id_shift = 3},
      TopologyInfo{.level_type = TopoLevel::Core, .logical_processors = 16, .id_shift = 5},
  };
  const std::array caches{
      CacheInfo{.level = 2, .type = CacheType::Unified, .threads_sharing = 2},
      CacheInfo{.level = 3, .type = CacheType::Unified, .threads_sharing = 32},
  };

  const SharingShifts shifts = sched::sharing_shifts(levels, caches);

  KEXPECT_EQ(shifts.cluster, 3u);
  KEXPECT_EQ(shifts.cache, 5u);
}

// There are parts that share an instruction cache more widely than their data caches. Nothing a
// thread would lose by moving depends on that one.
KTEST(Topology, instruction_caches_do_not_count) {
  const std::array levels{
      TopologyInfo{.level_type = TopoLevel::SMT, .logical_processors = 2, .id_shift = 1},
      TopologyInfo{.level_type = TopoLevel::Core, .logical_processors = 16, .id_shift = 5},
  };
  const std::array caches{
      CacheInfo{.level = 2, .type = CacheType::Unified, .threads_sharing = 2},
      CacheInfo{.level = 2, .type = CacheType::Instruction, .threads_sharing = 8},
      CacheInfo{.level = 3, .type = CacheType::Unified, .threads_sharing = 16},
      CacheInfo{.level = 4, .type = CacheType::Instruction, .threads_sharing = 32},
  };

  const SharingShifts shifts = sched::sharing_shifts(levels, caches);

  KEXPECT_EQ(shifts.cluster, 1u);
  KEXPECT_EQ(shifts.cache, 4u);
}

KTEST(Topology, what_a_cpu_does_not_say_is_as_wide_as_it_can_be) {
  const SharingShifts nothing = sched::sharing_shifts({}, {});

  KEXPECT_EQ(nothing.core, 0u);
  KEXPECT_EQ(nothing.cluster, 0u);
  KEXPECT_EQ(nothing.package, 32u);
  KEXPECT_EQ(nothing.cache, 32u);
}

// Hypervisors make up CPUID, and not always the same story in every leaf.
KTEST(Topology, the_groups_stay_nested_whatever_a_cpu_says) {
  const std::array levels{
      TopologyInfo{.level_type = TopoLevel::SMT, .logical_processors = 2, .id_shift = 2},
      TopologyInfo{.level_type = TopoLevel::Core, .logical_processors = 8, .id_shift = 3},
  };
  const std::array caches{
      CacheInfo{.level = 2, .type = CacheType::Unified, .threads_sharing = 1},
      CacheInfo{.level = 3, .type = CacheType::Unified, .threads_sharing = 256},
  };

  const SharingShifts shifts = sched::sharing_shifts(levels, caches);

  KEXPECT_LE(shifts.core, shifts.cluster);
  KEXPECT_LE(shifts.cluster, shifts.cache);
  KEXPECT_LE(shifts.cache, shifts.package);
}

KTEST(NumaDomainGroups, every_cpu_knows_its_groups) {
  Machine machine;

  KEXPECT(machine->group_of(13, Sharing::Core) == (SlotSpan{12, 2}));
  KEXPECT(machine->group_of(13, Sharing::Cluster) == (SlotSpan{12, 4}));
  KEXPECT(machine->group_of(13, Sharing::Cache) == (SlotSpan{8, 8}));
  KEXPECT(machine->group_of(13, Sharing::Package) == (SlotSpan{0, 16}));
  KEXPECT(machine->group_of(16, Sharing::Package) == (SlotSpan{16, 16}));
}

// The default for tables that do not mention these levels is one group, which should work out.
KTEST(NumaDomainGroups, levels_nobody_mentions_are_one_group) {
  NumaDomain domain;
  const std::array<CpuPlacement, 2> cpus{CpuPlacement{.cpu = 0, .core = 0},
                                         CpuPlacement{.cpu = 1, .core = 1}};

  KREQUIRE(domain.initialize(0, cpus, {}).has_value());

  KEXPECT(domain.group_of(1, Sharing::Core) == (SlotSpan{1, 1}));
  KEXPECT(domain.group_of(1, Sharing::Cache) == (SlotSpan{0, 2}));

  domain.release();
}

// Numbers a CPU reports can start over in every package, or line up across two of them.
KTEST(NumaDomainGroups, ids_only_have_to_be_unique_within_what_is_above_them) {
  NumaDomain domain;
  const std::array<CpuPlacement, 4> cpus{
      CpuPlacement{.cpu = 0, .core = 0, .cache = 0, .package = 0},
      CpuPlacement{.cpu = 1, .core = 1, .cache = 0, .package = 0},
      CpuPlacement{.cpu = 2, .core = 1, .cache = 0, .package = 1},
      CpuPlacement{.cpu = 3, .core = 0, .cache = 0, .package = 1},
  };

  KREQUIRE(domain.initialize(0, cpus, {}).has_value());

  KEXPECT_EQ(domain.core_count(), 4u);
  KEXPECT(domain.group_of(1, Sharing::Core) == (SlotSpan{1, 1}));
  KEXPECT(domain.group_of(1, Sharing::Cache) == (SlotSpan{0, 2}));
  KEXPECT(domain.group_of(2, Sharing::Cache) == (SlotSpan{2, 2}));

  domain.release();
}

KTEST(NumaDomainGroups, stealing_goes_for_the_closest_cpu_first) {
  Machine machine;

  // Everyone except the CPU looking has something to take.
  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_overloaded(slot, slot != 5);
  }

  const Visits visits = visits_from(machine.domain(), 5);

  KREQUIRE_EQ(visits.count, CPUS - 1);

  KEXPECT_EQ(visits.cpus[0], cpu_in_slot(4));  // Its SMT sibling.

  KEXPECT_EQ(visits.cpus[1], cpu_in_slot(6));  // The rest of the cluster.
  KEXPECT_EQ(visits.cpus[2], cpu_in_slot(7));

  for (std::uint32_t i = 3; i < 7; ++i) {  // The rest of the cache.
    KEXPECT_LT(visits.cpus[i], cpu_in_slot(4));
  }

  for (std::uint32_t i = 7; i < 15; ++i) {  // The rest of the package.
    KEXPECT_GE(visits.cpus[i], cpu_in_slot(8));
    KEXPECT_LT(visits.cpus[i], cpu_in_slot(16));
  }

  for (std::uint32_t i = 15; i < CPUS - 1; ++i) {  // The rest of the node.
    KEXPECT_GE(visits.cpus[i], cpu_in_slot(16));
  }
}

KTEST(NumaDomainGroups, stealing_goes_around_from_where_the_cpu_is) {
  Machine machine;

  for (std::uint32_t slot = 8; slot < 16; ++slot) {
    machine->set_overloaded(slot, true);
  }

  // Both from the same cache, and they should not start at the same victim.
  const Visits from_low = visits_from(machine.domain(), 0);
  const Visits from_high = visits_from(machine.domain(), 13);

  KEXPECT_EQ(from_low.cpus[0], cpu_in_slot(8));
  KEXPECT_EQ(from_high.cpus[0], cpu_in_slot(12));
}

KTEST(NumaDomainGroups, stealing_stops_when_told) {
  Machine machine;

  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_overloaded(slot, true);
  }

  std::uint32_t looked_at = 0;
  machine->for_each_overloaded_near(0, [&](const std::uint32_t) { return ++looked_at == 3; });

  KEXPECT_EQ(looked_at, 3u);
}

KTEST(NumaDomainGroups, nobody_gets_looked_at_twice) {
  Machine machine;

  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_overloaded(slot, true);
  }

  for (const std::uint32_t slot : {0u, 7u, 8u, 31u}) {
    const Visits visits = visits_from(machine.domain(), slot);
    KEXPECT_EQ(visits.count, CPUS - 1);

    std::uint64_t seen = 0;
    for (std::uint32_t i = 0; i < visits.count; ++i) {
      seen |= std::uint64_t{1} << (visits.cpus[i] - cpu_in_slot(0));
    }

    KEXPECT_EQ(seen, ((std::uint64_t{1} << CPUS) - 1) & ~(std::uint64_t{1} << slot));
  }
}

KTEST(NumaDomainGroups, a_whole_core_in_the_cache_goes_first) {
  Machine machine;

  // In the cache of slots 8-15, slots 8 to 12 are idle, which leaves whole cores at 8 and 10, and
  // a half-busy one at 12. Everything in the next cache is idle, and does not matter.
  machine->mark_idle(8);
  machine->mark_idle(9);
  machine->mark_idle(12);

  KEXPECT_EQ(machine->claim_near(14), cpu_in_slot(8));
}

KTEST(NumaDomainGroups, a_sibling_in_the_cache_beats_a_core_elsewhere) {
  Machine machine;

  machine->mark_idle(0);
  machine->mark_idle(1);
  machine->mark_idle(12);

  KEXPECT_EQ(machine->claim_near(14), cpu_in_slot(12));
}

KTEST(NumaDomainGroups, nothing_in_the_cache_means_nothing_near) {
  Machine machine;

  machine->mark_idle(0);
  KEXPECT_EQ(machine->claim_near(14), NO_CPU);
}

// Claims are what keep two threads off one CPU, so this has to go by the same rules as the others.
KTEST(NumaDomainGroups, a_cpu_claimed_nearby_is_gone) {
  Machine machine;

  machine->mark_idle(12);

  KEXPECT_EQ(machine->claim_near(14), cpu_in_slot(12));
  KEXPECT_EQ(machine->claim_near(14), NO_CPU);
  KEXPECT_EQ(machine->claim_idle_cpu(), NO_CPU);
  KEXPECT_EQ(machine->idle_cpu_count(), 0u);
}

KTEST(NumaDomainCapacity, the_same_for_everyone_changes_nothing) {
  Machine machine;

  machine.all_idle();

  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_capacity(slot, 300);
  }

  KEXPECT_EQ(machine->capacity_of(4), 300u);
  KEXPECT_EQ(machine->claim_idle_cpu(), cpu_in_slot(0));
}

KTEST(NumaDomainCapacity, the_fastest_idle_cpu_goes_first) {
  Machine machine;

  machine.all_idle();

  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_capacity(slot, slot >= 20 ? FULL_CAPACITY : FULL_CAPACITY / 2);
  }

  KEXPECT_EQ(machine->claim_idle_cpu(), cpu_in_slot(20));
  KEXPECT_EQ(machine->claim_idle_core(), cpu_in_slot(22));
}

KTEST(NumaDomainCapacity, the_slow_ones_are_better_than_nothing) {
  Machine machine;

  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    machine->set_capacity(slot, slot >= 20 ? FULL_CAPACITY : FULL_CAPACITY / 2);
  }

  machine->mark_idle(3);

  KEXPECT_EQ(machine->claim_idle_cpu(), cpu_in_slot(3));
  KEXPECT_EQ(machine->claim_idle_cpu(), NO_CPU);
}

KTEST(NumaDomainCapacity, near_means_a_fast_cpu_if_there_is_one) {
  Machine machine;

  // All in one cache, the way it is on a hybrid part.
  for (std::uint32_t slot = 8; slot < 16; ++slot) {
    machine->mark_idle(slot);
    machine->set_capacity(slot, slot >= 12 ? FULL_CAPACITY : FULL_CAPACITY / 2);
  }

  KEXPECT_EQ(machine->claim_near(8), cpu_in_slot(12));
}

// From here on, the machine this is running on.
KTEST(MachineTopology, groups_are_nested_and_cover_the_node) {
  for (std::uint32_t node = 0; node < smp::node_count(); ++node) {
    const NumaDomain& domain = sched::Scheduler::numa_domain(node);

    for (std::uint32_t slot = 0; slot < domain.cpu_count(); ++slot) {
      SlotSpan inner{slot, 1};

      for (const Sharing level :
           {Sharing::Core, Sharing::Cluster, Sharing::Cache, Sharing::Package}) {
        const SlotSpan group = domain.group_of(slot, level);

        KEXPECT(group.contains(slot));
        KEXPECT_LE(group.first, inner.first);
        KEXPECT_GE(group.first + group.count, inner.first + inner.count);
        KEXPECT_LE(group.first + group.count, domain.cpu_count());
        inner = group;
      }
    }
  }
}

KTEST(MachineTopology, every_cpu_has_the_slot_its_domain_says) {
  for (std::uint32_t cpu = 0; cpu < smp::online_cpus(); ++cpu) {
    const smp::CpuEntry& entry = smp::entry(cpu);
    const NumaDomain& domain = sched::Scheduler::numa_domain(entry.numa_node);

    KEXPECT_EQ(domain.cpu_at(entry.node_slot), cpu);
  }
}

KTEST(MachineTopology, capacities_are_in_range_and_someone_is_at_the_top) {
  std::uint32_t top = 0;

  for (std::uint32_t cpu = 0; cpu < smp::online_cpus(); ++cpu) {
    const sched::RunQueue& rq = sched::Scheduler::cpu_runqueue(cpu);

    KEXPECT_GT(rq.capacity(), 0u);
    KEXPECT_LE(rq.capacity(), FULL_CAPACITY);
    KEXPECT_EQ(rq.domain()->capacity_of(rq.slot()), rq.capacity());
    top = std::max(top, rq.capacity());
  }

  KEXPECT_EQ(top, FULL_CAPACITY);
}

// How the CPUs compare is only known at the very end of bringing them up, and has to get from each
// run queue to the node. This pretends that one of them turned out slower, then takes that back.
KTEST(MachineTopology, what_a_cpu_can_do_reaches_its_domain) {
  if (smp::online_cpus() < 2) {
    KTEST_SKIP("nothing to compare it to");
  }

  sched::RunQueue& rq = sched::Scheduler::cpu_runqueue(support::last_cpu());

  rq.set_capacity(FULL_CAPACITY / 2);
  sched::balance::publish_capacities();

  KEXPECT_EQ(rq.capacity(), FULL_CAPACITY / 2);
  KEXPECT_EQ(rq.domain()->capacity_of(rq.slot()), FULL_CAPACITY / 2);

  rq.set_capacity(FULL_CAPACITY);
  sched::balance::publish_capacities();

  KEXPECT_EQ(rq.domain()->capacity_of(rq.slot()), FULL_CAPACITY);
}

namespace {
// Sleeps until woken from outside, and says where it was before and after that.
struct Traveller {
  std::atomic<sched::Thread*> self{nullptr};
  std::atomic<std::uint32_t> cpu_before{NumaDomain::NO_CPU};
  std::atomic<std::uint32_t> cpu_after{NumaDomain::NO_CPU};
  std::atomic<bool> done{false};
};

void nap_and_report(Traveller* traveller) noexcept {
  traveller->cpu_before.store(hw::percpu::id(), std::memory_order_release);
  traveller->self.store(sched::current(), std::memory_order_release);

  (void)sched::sleep_for(klib::Duration::seconds(60));

  traveller->cpu_after.store(hw::percpu::id(), std::memory_order_release);
  traveller->done.store(true, std::memory_order_release);
}

struct Spinner {
  klib::Duration time{};
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
};

void keep_the_cpu(Spinner* spinner) noexcept {
  spinner->started.store(true, std::memory_order_release);
  support::spin_for(spinner->time);
  spinner->done.store(true, std::memory_order_release);
}
}  // namespace

// Only means something on a machine with more than one cache. The stress matrix has one of those.
// Where a thread starts is up to the scheduler, so this goes by where it did.
KTEST(MachineTopology, a_thread_wakes_up_near_where_it_ran) {
  Traveller traveller;
  sched::ThreadCreateOptions options{};
  options.preferred_cpu = support::last_cpu();

  KREQUIRE(support::start("traveller", &nap_and_report, &traveller, options) != nullptr);
  KREQUIRE(support::wait_for([&] {
    const sched::Thread* thread = traveller.self.load(std::memory_order_acquire);
    return thread != nullptr && thread->state() == sched::ThreadState::Sleeping;
  }));

  const std::uint32_t cpu = traveller.cpu_before.load();
  const smp::CpuEntry& entry = smp::entry(cpu);
  const NumaDomain& domain = sched::Scheduler::numa_domain(entry.numa_node);
  const SlotSpan cache = domain.group_of(entry.node_slot, Sharing::Cache);
  const bool fits =
      cache.count >= 2 && cache.count != domain.cpu_count() && cpu != hw::percpu::id();

  // Taken by the time it wakes up, so it has to go somewhere else, and the rest are all idle.
  Spinner spinner{.time = klib::Duration::milliseconds(20)};
  if (fits) {
    KREQUIRE(support::start("spinner", &keep_the_cpu, &spinner, support::pinned_to(cpu)) !=
             nullptr);
    KREQUIRE(support::wait_for([&] { return spinner.started.load(std::memory_order_acquire); }));
  }

  sched::cancel_sleep(traveller.self.load());

  KEXPECT(support::wait_for([&] { return traveller.done.load(std::memory_order_acquire); }));
  if (!fits) {
    KTEST_SKIP("takes a cache with room in it, and CPUs outside of it");
  }

  KEXPECT(support::wait_for([&] { return spinner.done.load(std::memory_order_acquire); }));

  const std::uint32_t landed = traveller.cpu_after.load();
  KEXPECT_NE(landed, NumaDomain::NO_CPU);
  KEXPECT(smp::entry(landed).numa_node != entry.numa_node ||
          cache.contains(smp::entry(landed).node_slot));
}

namespace {
// Runs until told to stop, keeping track of where.
struct Runner {
  std::atomic<std::uint32_t> cpu{NumaDomain::NO_CPU};
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
};

// Never keeps the CPU the test runs on to itself: the test would not get it back to say stop.
void run_and_report(Runner* runner) noexcept {
  while (!runner->stop.load(std::memory_order_acquire)) {
    const std::uint32_t cpu = hw::percpu::id();
    runner->cpu.store(cpu, std::memory_order_release);

    if (cpu == 0) {
      (void)sched::sleep_for(klib::Duration::microseconds(100));
    } else {
      hw::pause();
    }
  }

  runner->done.store(true, std::memory_order_release);
}
}  // namespace

// A thread loses its CPU to a real-time thread, with everything around it taken as well. What is
// idle is asleep in another cache, where nothing would notice, and has to be told. Where the thread
// starts is up to the scheduler, so this goes by where it did.
KTEST(MachineTopology, work_spills_over_to_another_cache) {
  static constexpr std::uint32_t MAX_SPINNERS = 8;

  // Long enough to tell moving apart from waiting, even with the host taking CPUs away.
  static constexpr klib::Duration TAKEN_FOR = klib::Duration::seconds(2);

  if (smp::online_cpus() < 2) {
    KTEST_SKIP("takes caches to choose between");
  }

  Runner runner;
  sched::ThreadCreateOptions options{};
  options.preferred_cpu = support::last_cpu();
  KREQUIRE(support::start("runner", &run_and_report, &runner, options) != nullptr);
  KREQUIRE(support::wait_for(
      [&] { return runner.cpu.load(std::memory_order_acquire) != NumaDomain::NO_CPU; }));

  const std::uint32_t cpu = runner.cpu.load();
  const smp::CpuEntry& entry = smp::entry(cpu);
  const NumaDomain& domain = sched::Scheduler::numa_domain(entry.numa_node);
  const SlotSpan cache = domain.group_of(entry.node_slot, Sharing::Cache);

  if (cache.count == domain.cpu_count() || cache.count > MAX_SPINNERS ||
      cache.contains(smp::entry(hw::percpu::id()).node_slot)) {
    runner.stop.store(true, std::memory_order_release);
    KEXPECT(support::wait_for([&] { return runner.done.load(std::memory_order_acquire); }));
    KTEST_SKIP("takes a cache of its own, with CPUs left outside of it");
  }

  std::array<Spinner, MAX_SPINNERS> spinners;
  for (Spinner& spinner : spinners) {
    spinner.time = TAKEN_FOR;
  }

  // Everything in the cache except the runner's CPU. From here on nothing may return early: these
  // threads have to be seen to the end.
  std::uint32_t started = 0;
  for (std::uint32_t slot = cache.first; slot < cache.first + cache.count; ++slot) {
    if (domain.cpu_at(slot) != cpu) {
      const auto* spinner =
          support::start("taken", &keep_the_cpu, &spinners[started],
                         support::of_class(sched::PriorityClass::RealTime, domain.cpu_at(slot)));
      KEXPECT(spinner != nullptr);
      started += spinner != nullptr;
    }
  }

  for (std::uint32_t i = 0; i < started; ++i) {
    KEXPECT(support::wait_for([&] { return spinners[i].started.load(std::memory_order_acquire); }));
  }

  // Let everything else that is idle get to sleep, or it would come and look by itself.
  support::spin_for(klib::Duration::milliseconds(5));

  Spinner usurper{.time = TAKEN_FOR};
  const auto* taken = support::start("usurper", &keep_the_cpu, &usurper,
                                     support::of_class(sched::PriorityClass::RealTime, cpu));
  KEXPECT(taken != nullptr);

  if (taken != nullptr) {
    KEXPECT(support::wait_for([&] { return usurper.started.load(std::memory_order_acquire); }));
    KEXPECT(
        support::wait_for([&] { return !cache.contains(smp::entry(runner.cpu.load()).node_slot); },
                          klib::Duration::seconds(1)));
    KEXPECT(!usurper.done.load(std::memory_order_acquire));
  }

  runner.stop.store(true, std::memory_order_release);
  KEXPECT(support::wait_for([&] { return runner.done.load(std::memory_order_acquire); }));

  if (taken != nullptr) {
    KEXPECT(support::wait_for([&] { return usurper.done.load(std::memory_order_acquire); }));
  }

  for (std::uint32_t i = 0; i < started; ++i) {
    KEXPECT(support::wait_for([&] { return spinners[i].done.load(std::memory_order_acquire); }));
  }
}

KTEST(MachineTopology, what_cpuid_says_is_in_order) {
  const SharingShifts shifts = sched::sharing_shifts(*hw::percpu::self()->info);

  KEXPECT_LE(shifts.core, shifts.cluster);
  KEXPECT_LE(shifts.cluster, shifts.cache);
  KEXPECT_LE(shifts.cache, shifts.package);
}
}  // namespace kernel::testing
