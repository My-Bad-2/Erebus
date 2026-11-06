#include "sched/numa_domain.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "ktest.hpp"

namespace kernel::testing {
namespace {
using sched::CpuPlacement;
using sched::NumaDomain;

constexpr std::uint32_t CPU_BASE = 100;
constexpr std::uint32_t CPUS = 130;  // Enough to need more than one word of every bitmap.
constexpr std::uint32_t NO_CPU = NumaDomain::NO_CPU;

// Two threads per core, with CPU numbers that have nothing to do with the slots.
void build_two_thread_domain(NumaDomain& domain) noexcept {
  std::array<CpuPlacement, CPUS> cpus{};
  for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
    cpus[slot] = {.cpu = CPU_BASE + slot, .core = slot / 2};
  }

  const std::array<std::uint32_t, 2> neighbors{5, 3};
  (void)domain.initialize(0, cpus, neighbors);
}

class TwoThreadDomain {
  NumaDomain m_domain;

 public:
  TwoThreadDomain() noexcept { build_two_thread_domain(m_domain); }
  ~TwoThreadDomain() noexcept { m_domain.release(); }

  [[nodiscard]] NumaDomain* operator->() noexcept { return &m_domain; }
};

constinit NumaDomain g_shared;
std::atomic<std::uint32_t> g_claims{0};
std::atomic<std::uint32_t> g_duplicates{0};
std::array<std::atomic<std::uint8_t>, CPUS> g_claimed_slots{};
}  // namespace

KTEST(NumaDomain, describes_the_node_it_was_built_for) {
  TwoThreadDomain domain;

  KEXPECT_EQ(domain->cpu_count(), CPUS);
  KEXPECT_EQ(domain->core_count(), 65u);
  KEXPECT_EQ(domain->cpu_at(0), CPU_BASE);
  KEXPECT_EQ(domain->cpu_at(CPUS - 1), CPU_BASE + CPUS - 1);

  KREQUIRE_EQ(domain->neighbors().size(), 2u);
  KEXPECT_EQ(domain->neighbors()[0], 5u);
  KEXPECT_EQ(domain->neighbors()[1], 3u);
}

KTEST(NumaDomain, a_node_without_cpus_has_nothing_to_offer) {
  NumaDomain domain;
  KREQUIRE(domain.initialize(0, {}, {}).has_value());

  KEXPECT_EQ(domain.claim_idle_cpu(), NO_CPU);
  KEXPECT_EQ(domain.claim_idle_core(), NO_CPU);
  domain.for_each_overloaded(0, [](std::uint32_t) {
    KEXPECT(false);
    return true;
  });

  domain.release();
}

KTEST(NumaDomain, nobody_is_idle_to_begin_with) {
  TwoThreadDomain domain;

  KEXPECT_EQ(domain->claim_idle_cpu(), NO_CPU);
  KEXPECT_EQ(domain->claim_idle_core(), NO_CPU);
  KEXPECT(!domain->claim_cpu(7));
}

KTEST(NumaDomain, an_idle_cpu_can_be_claimed_once) {
  TwoThreadDomain domain;

  domain->mark_idle(7);
  domain->mark_idle(70);
  KEXPECT_EQ(domain->idle_cpu_count(), 2u);
  KEXPECT(domain->claim_cpu(70));

  KEXPECT(domain->claim_cpu(7));
  KEXPECT(!domain->claim_cpu(7));
  KEXPECT_EQ(domain->claim_idle_cpu(), NO_CPU);
  KEXPECT_EQ(domain->idle_cpu_count(), 0u);
}

KTEST(NumaDomain, any_idle_cpu_will_do_in_any_word) {
  TwoThreadDomain domain;

  domain->mark_idle(129);
  KEXPECT_EQ(domain->claim_idle_cpu(), CPU_BASE + 129);

  domain->mark_idle(3);
  domain->mark_idle(70);
  KEXPECT_EQ(domain->claim_idle_cpu(), CPU_BASE + 3);
  KEXPECT_EQ(domain->claim_idle_cpu(), CPU_BASE + 70);
  KEXPECT_EQ(domain->claim_idle_cpu(), NO_CPU);
}

KTEST(NumaDomain, a_core_is_idle_when_all_of_it_is) {
  TwoThreadDomain domain;

  domain->mark_idle(4);
  KEXPECT_EQ(domain->claim_idle_core(), NO_CPU);

  domain->mark_idle(5);
  const std::uint32_t cpu = domain->claim_idle_core();
  KEXPECT(cpu == CPU_BASE + 4 || cpu == CPU_BASE + 5);

  // The sibling is still free, but the core is taken now.
  KEXPECT_EQ(domain->claim_idle_core(), NO_CPU);
  KEXPECT_NE(domain->claim_idle_cpu(), NO_CPU);
}

KTEST(NumaDomain, a_whole_core_beats_half_a_core) {
  TwoThreadDomain domain;

  domain->mark_idle(0);  // Next to something busy.
  domain->mark_idle(8);
  domain->mark_idle(9);

  const std::uint32_t cpu = domain->claim_idle_core();
  KEXPECT(cpu == CPU_BASE + 8 || cpu == CPU_BASE + 9);
}

KTEST(NumaDomain, a_claimed_sibling_takes_the_core_with_it) {
  TwoThreadDomain domain;

  domain->mark_idle(10);
  domain->mark_idle(11);
  KEXPECT(domain->claim_cpu(10));

  KEXPECT_EQ(domain->claim_idle_core(), NO_CPU);
  KEXPECT_EQ(domain->claim_idle_cpu(), CPU_BASE + 11);
}

KTEST(NumaDomain, going_idle_twice_counts_once) {
  TwoThreadDomain domain;

  // The idle loop publishes on every pass. A count that went down each time would call the core
  // idle with its sibling still busy.
  domain->mark_idle(12);
  domain->mark_idle(12);
  domain->mark_idle(12);

  KEXPECT_EQ(domain->claim_idle_core(), NO_CPU);

  domain->mark_busy(12);
  domain->mark_busy(12);
  domain->mark_idle(12);
  domain->mark_idle(13);
  KEXPECT_NE(domain->claim_idle_core(), NO_CPU);
}

KTEST(NumaDomain, a_core_can_go_around_as_often_as_it_likes) {
  TwoThreadDomain domain;

  for (std::uint32_t round = 0; round < 100; ++round) {
    domain->mark_idle(20);
    domain->mark_idle(21);

    const std::uint32_t first = domain->claim_idle_core();
    KREQUIRE_NE(first, NO_CPU);
    KEXPECT_NE(domain->claim_idle_cpu(), NO_CPU);
    KEXPECT_EQ(domain->claim_idle_cpu(), NO_CPU);
  }
}

KTEST(NumaDomain, overloaded_cpus_are_found_in_every_word) {
  TwoThreadDomain domain;
  domain->set_overloaded(1, true);
  domain->set_overloaded(64, true);
  domain->set_overloaded(129, true);

  std::array<std::uint32_t, 4> found{};
  std::size_t count = 0;
  domain->for_each_overloaded(0, [&](const std::uint32_t cpu) {
    found[count++] = cpu;
    return false;
  });

  KREQUIRE_EQ(count, 3u);
  KEXPECT_EQ(found[0], CPU_BASE + 1);
  KEXPECT_EQ(found[1], CPU_BASE + 64);
  KEXPECT_EQ(found[2], CPU_BASE + 129);

  domain->set_overloaded(64, false);

  count = 0;
  domain->for_each_overloaded(0, [&](const std::uint32_t cpu) {
    found[count++] = cpu;
    return false;
  });
  KEXPECT_EQ(count, 2u);
}

KTEST(NumaDomain, the_search_for_work_starts_where_it_is_told_and_stops_when_told) {
  TwoThreadDomain domain;
  domain->set_overloaded(1, true);
  domain->set_overloaded(70, true);

  std::uint32_t first = NO_CPU;
  std::uint32_t visits = 0;
  domain->for_each_overloaded(65, [&](const std::uint32_t cpu) {
    first = cpu;
    ++visits;
    return true;
  });

  KEXPECT_EQ(visits, 1u);
  KEXPECT_EQ(first, CPU_BASE + 70);
}

KTEST_ALL_CPUS(NumaDomain, every_idle_cpu_goes_to_exactly_one_claimer, cpu) {
  if (cpu == 0) {
    build_two_thread_domain(g_shared);
    g_claims.store(0, std::memory_order_relaxed);
    g_duplicates.store(0, std::memory_order_relaxed);
  }

  sync_cpus();

  for (std::uint32_t round = 0; round < 200; ++round) {
    if (cpu == 0) {
      for (auto& claimed : g_claimed_slots) {
        claimed.store(0, std::memory_order_relaxed);
      }

      for (std::uint32_t slot = 0; slot < CPUS; ++slot) {
        g_shared.mark_idle(slot);
      }
    }

    sync_cpus();

    while (true) {
      const std::uint32_t claimed = g_shared.claim_idle_cpu();
      if (claimed == NO_CPU) {
        break;
      }

      g_claims.fetch_add(1, std::memory_order_relaxed);
      if (g_claimed_slots[claimed - CPU_BASE].exchange(1, std::memory_order_relaxed) != 0) {
        g_duplicates.fetch_add(1, std::memory_order_relaxed);
      }
    }

    sync_cpus();
  }

  if (cpu == 0) {
    KEXPECT_EQ(g_duplicates.load(std::memory_order_relaxed), 0u);
    KEXPECT_EQ(g_claims.load(std::memory_order_relaxed), 200 * CPUS);
    g_shared.release();
  }
}
}  // namespace kernel::testing
