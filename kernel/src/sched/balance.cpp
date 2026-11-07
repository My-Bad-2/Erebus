#include "sched/balance.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <tuple>

#include "drivers/clock.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "sched/scheduler.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::sched {
namespace {
namespace smp = hw::smp;

constexpr std::uint32_t MAX_STEAL_ATTEMPTS = 8;
constexpr std::uint32_t REMOTE_STEAL_THRESHOLD = 2;

// Everything a CPU can be told apart by, most significant first. An APIC ID is 32 bits at most,
// and a shift that takes all of them means that there is one group.
[[nodiscard]] constexpr std::uint32_t group_id(std::uint32_t apic_id,
                                               std::uint32_t shift) noexcept {
  return (shift < 32) ? apic_id >> shift : 0;
}

struct CpuRecord {
  std::uint32_t node;
  std::uint32_t package;
  std::uint32_t cache;
  std::uint32_t cluster;
  std::uint32_t core;
  std::uint32_t cpu;
};

[[nodiscard]] std::uint32_t claim_in(NumaDomain& domain) noexcept {
  const std::uint32_t cpu = domain.claim_idle_core();
  return cpu != NumaDomain::NO_CPU ? cpu : domain.claim_idle_cpu();
}

// The less busy of two samples of @p home, where @p favorite gets to be one of them if it can.
[[nodiscard]] std::uint32_t less_busy_cpu(const NumaDomain& home, std::uint32_t favorite) noexcept {
  RunQueue& self = hw::percpu::runqueue();

  if (home.cpu_count() == 0) {
    return smp::is_online(favorite) ? favorite : self.cpu_id();
  }

  const auto sample = [&] { return home.cpu_at(self.next_random() % home.cpu_count()); };

  const std::uint32_t first = smp::is_online(favorite) ? favorite : sample();
  const std::uint32_t second = sample();

  if (!smp::is_online(first)) {
    return smp::is_online(second) ? second : self.cpu_id();
  }

  if (!smp::is_online(second)) {
    return first;
  }

  const RunQueue& a = Scheduler::cpu_runqueue(first);
  const RunQueue& b = Scheduler::cpu_runqueue(second);
  return is_less_busy(b.load(), b.capacity(), a.load(), a.capacity()) ? second : first;
}

// Tries the overloaded CPUs near @p slot, as long as @p attempts lasts.
[[nodiscard]] Thread* steal_near(const NumaDomain& domain, std::uint32_t slot, std::uint32_t self,
                                 std::uint32_t& attempts) noexcept {
  Thread* stolen = nullptr;

  domain.for_each_overloaded_near(slot, [&](const std::uint32_t cpu) {
    if (cpu == self || !smp::is_online(cpu)) {
      return false;
    }

    stolen = Scheduler::cpu_runqueue(cpu).try_steal(1, self);
    return stolen != nullptr || --attempts == 0;
  });

  return stolen;
}

// Tries the overloaded CPUs of a node that is not ours, which have to be worth the trip.
[[nodiscard]] Thread* steal_from_afar(const NumaDomain& domain, std::uint32_t self,
                                      std::uint32_t& attempts) noexcept {
  Thread* stolen = nullptr;

  domain.for_each_overloaded(0, [&](const std::uint32_t cpu) {
    if (cpu == self || !smp::is_online(cpu)) {
      return false;
    }

    stolen = Scheduler::cpu_runqueue(cpu).try_steal(REMOTE_STEAL_THRESHOLD, self);
    return stolen != nullptr || --attempts == 0;
  });

  return stolen;
}
}  // namespace

SharingShifts sharing_shifts(const std::span<const hw::TopologyInfo> levels,
                             const std::span<const hw::CacheInfo> caches) noexcept {
  SharingShifts shifts{};

  std::uint32_t module_shift = 0;
  for (const hw::TopologyInfo& level : levels) {
    if (level.level_type == hw::TopoLevel::SMT) {
      shifts.core = level.id_shift;
    } else if (level.level_type == hw::TopoLevel::Module) {
      module_shift = level.id_shift;
    }
  }

  // Whatever the last level is, what is above it is the package. Saying nothing at all means that
  // a CPU has no idea, and then it all counts as one.
  shifts.package = !levels.empty() && levels.back().id_shift != 0 ? levels.back().id_shift : 32;

  std::uint32_t mid_level_shift = 0;
  std::uint32_t last_level = 0;
  std::uint32_t last_level_shift = shifts.package;

  for (const hw::CacheInfo& cache : caches) {
    if (cache.type == hw::CacheType::Instruction) {
      continue;
    }

    const auto shift = static_cast<std::uint32_t>(std::bit_width(cache.threads_sharing - 1));
    if (cache.level == 2) {
      mid_level_shift = shift;
    }

    if (cache.level > last_level) {
      last_level = cache.level;
      last_level_shift = shift;
    }
  }

  // CPUID in a VM is whatever someone configured, which does not have to add up, and everything
  // after this relies on each group fitting in the next.
  shifts.package = std::max(shifts.package, shifts.core);
  shifts.cache = std::clamp(last_level_shift, shifts.core, shifts.package);
  shifts.cluster = std::clamp(std::max(mid_level_shift, module_shift), shifts.core, shifts.cache);
  return shifts;
}

namespace balance {
// Every node's domain, with slots sorted by what the CPUs share, so that any group of them ends up
// in one piece.
void build_domains() noexcept {
  const std::uint32_t cpus = smp::possible_cpus();
  const std::uint32_t nodes = smp::node_count();
  const SharingShifts shifts = sharing_shifts(*hw::percpu::self()->info);

  auto* records = static_cast<CpuRecord*>(memory::heap::kmalloc(sizeof(CpuRecord) * cpus));
  auto* placements = static_cast<CpuPlacement*>(memory::heap::kmalloc(sizeof(CpuPlacement) * cpus));
  auto* neighbors =
      static_cast<std::uint32_t*>(memory::heap::kmalloc(sizeof(std::uint32_t) * nodes));
  auto* node_cpus =
      static_cast<std::uint32_t*>(memory::heap::kmalloc(sizeof(std::uint32_t) * nodes));
  if (!records || !placements || !neighbors || !node_cpus) {
    utils::logger::fatal("sched: out of memory for the topology of {} CPUs\n", cpus);
  }

  std::fill_n(node_cpus, nodes, 0u);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    const smp::CpuEntry& entry = smp::entry(cpu);
    records[cpu] = {.node = entry.numa_node,
                    .package = group_id(entry.apic_id, shifts.package),
                    .cache = group_id(entry.apic_id, shifts.cache),
                    .cluster = group_id(entry.apic_id, shifts.cluster),
                    .core = group_id(entry.apic_id, shifts.core),
                    .cpu = cpu};
    ++node_cpus[entry.numa_node];
  }

  const std::span sorted{records, cpus};
  std::ranges::sort(sorted, [](const CpuRecord& a, const CpuRecord& b) {
    return std::tie(a.node, a.package, a.cache, a.cluster, a.core, a.cpu) <
           std::tie(b.node, b.package, b.cache, b.cluster, b.core, b.cpu);
  });

  std::uint32_t next_record = 0;
  for (std::uint32_t node = 0; node < nodes; ++node) {
    const std::uint32_t count = node_cpus[node];
    for (std::uint32_t slot = 0; slot < count; ++slot) {
      const CpuRecord& record = records[next_record++];
      placements[slot] = {.cpu = record.cpu,
                          .core = record.core,
                          .cluster = record.cluster,
                          .cache = record.cache,
                          .package = record.package};
      smp::entry(record.cpu).node_slot = slot;
    }

    // SLIT distances from here to every other node that has a CPU to run on.
    std::uint32_t neighbor_count = 0;
    for (std::uint32_t other = 0; other < nodes; ++other) {
      if (other != node && node_cpus[other] != 0) {
        neighbors[neighbor_count++] = other;
      }
    }

    const std::span nearby{neighbors, neighbor_count};
    std::ranges::sort(nearby, {}, [node](const std::uint32_t other) {
      return memory::pmm::g_topology.get_latency(node, other);
    });

    NumaDomain& domain = smp::node_area(node)->scheduler;
    if (!domain.initialize(node, {placements, count}, nearby)) {
      utils::logger::fatal("sched: out of memory for the domain of node {}\n", node);
    }

    std::uint32_t caches = 0;
    for (std::uint32_t slot = 0; slot < count; ++slot) {
      caches += domain.group_of(slot, Sharing::Cache).first == slot;
    }

    utils::logger::info("sched: node {}: {} CPUs, {} cores, {} last-level caches\n", node, count,
                        domain.core_count(), caches);
  }

  memory::heap::kfree(node_cpus);
  memory::heap::kfree(neighbors);
  memory::heap::kfree(placements);
  memory::heap::kfree(records);
}

void publish_capacities() noexcept {
  const std::uint32_t cpus = smp::possible_cpus();

  std::uint32_t fastest = 0;
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    if (smp::is_online(cpu)) {
      fastest = std::max(fastest, Scheduler::cpu_runqueue(cpu).capacity());
    }
  }

  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    if (smp::is_online(cpu)) {
      RunQueue& rq = Scheduler::cpu_runqueue(cpu);
      const std::uint32_t capacity = scale_capacity(rq.capacity(), fastest);

      rq.set_capacity(capacity);
      if (NumaDomain* domain = rq.domain(); domain != nullptr) {
        domain->set_capacity(rq.slot(), capacity);
      }
    }
  }
}

std::uint32_t select_cpu(const Thread& thread) noexcept {
  const std::uint32_t last = thread.cpu_id();
  if (thread.is_pinned()) {
    return last;
  }

  const bool last_online = smp::is_online(last);

  std::uint32_t node = thread.preferred_numa_node();
  if (node >= smp::node_count()) {
    node = last_online ? smp::entry(last).numa_node : hw::percpu::numa_node_id();
  }

  NumaDomain& home = Scheduler::numa_domain(node);

  const bool last_is_home = last_online && smp::entry(last).numa_node == node;
  if (last_is_home) {
    const std::uint32_t slot = smp::entry(last).node_slot;
    if (home.claim_cpu(slot)) {
      return last;
    }

    if (const std::uint32_t cpu = home.claim_near(slot); cpu != NumaDomain::NO_CPU) {
      return cpu;
    }
  }

  if (const std::uint32_t cpu = claim_in(home); cpu != NumaDomain::NO_CPU) {
    return cpu;
  }

  for (const std::uint32_t neighbor : home.neighbors()) {
    if (const std::uint32_t cpu = claim_in(Scheduler::numa_domain(neighbor));
        cpu != NumaDomain::NO_CPU) {
      return cpu;
    }
  }

  return less_busy_cpu(home, last_is_home ? last : smp::INVALID_CPU);
}

// A CPU only goes looking for work on its way to sleep, so one that is asleep already has to be
// told. That goes for every way a queue can grow: a new thread, one that lost the CPU to a
// real-time thread, or one that picked its CPU just before another one went idle.
//
// This takes a real IPI wherever the CPU sleeps, as opposed to hw::kick_idle_cpu(). A CPU in MWAIT
// is watching its own queue, and nothing is changing that here: the work is on somebody else's.
void offer_spare_work(RunQueue& rq) noexcept {
  const std::uint32_t spare = rq.spare();
  NumaDomain* home = rq.domain();
  if (spare == 0 || home == nullptr) {
    return;
  }

  std::uint32_t helper = home->claim_near(rq.slot());

  if (helper == NumaDomain::NO_CPU) {
    helper = claim_in(*home);
  }

  if (helper == NumaDomain::NO_CPU && spare >= REMOTE_STEAL_THRESHOLD) {
    for (const std::uint32_t node : home->neighbors()) {
      helper = Scheduler::numa_domain(node).claim_idle_cpu();
      if (helper != NumaDomain::NO_CPU) {
        break;
      }
    }
  }

  if (helper != NumaDomain::NO_CPU) {
    hw::ipi::send_reschedule(helper);
  }
}

bool steal_work(RunQueue& rq) noexcept {
  utils::IrqDisableGuard irq_guard;

  if (rq.domain() == nullptr) {
    return false;
  }

  std::uint32_t attempts = MAX_STEAL_ATTEMPTS;
  const NumaDomain& home = *rq.domain();
  Thread* stolen = steal_near(home, rq.slot(), rq.cpu_id(), attempts);

  for (const std::uint32_t node : home.neighbors()) {
    if (stolen != nullptr || attempts == 0) {
      break;
    }

    stolen = steal_from_afar(Scheduler::numa_domain(node), rq.cpu_id(), attempts);
  }

  if (stolen == nullptr) {
    return false;
  }

  rq.lock();
  rq.update_clock_locked(drivers::SystemClock::now().raw_ns());
  (void)rq.enqueue_locked(*stolen);
  rq.unlock();
  return true;
}
}  // namespace balance
}  // namespace kernel::sched
