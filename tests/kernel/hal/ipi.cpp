#include "hal/smp/ipi.hpp"

#include <atomic>

#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
namespace ipi = hw::ipi;
namespace smp = hw::smp;

// How often a call ran on each CPU; MAX_CPUS entries would be 512 KiB, so this covers a
// machine of up to 1024 CPUs and the tests skip beyond that.
constexpr std::uint32_t TRACKED_CPUS = 1024;
std::atomic<std::uint64_t> g_hits[TRACKED_CPUS];

std::atomic<std::uint64_t> g_async_sent{0};
std::atomic<std::uint64_t> g_async_ran{0};
std::atomic<std::uint32_t> g_saw_interrupts_on{0};
std::atomic<std::uint32_t> g_saw_stale_data{0};

// Written before a call, read by the targets: a call must see what its caller wrote.
std::uint64_t g_payload = 0;

void count_hit(void*) noexcept {
  g_hits[hw::percpu::id()].fetch_add(1, std::memory_order_relaxed);
  if (hw::irq::is_enabled()) {
    g_saw_interrupts_on.fetch_add(1, std::memory_order_relaxed);
  }
}

void count_async(void*) noexcept { g_async_ran.fetch_add(1, std::memory_order_relaxed); }

void record_cpu(void* arg) noexcept {
  static_cast<std::atomic<std::uint32_t>*>(arg)->store(hw::percpu::id(), std::memory_order_relaxed);
}

void check_payload(void* arg) noexcept {
  if (g_payload != *static_cast<const std::uint64_t*>(arg)) {
    g_saw_stale_data.fetch_add(1, std::memory_order_relaxed);
  }
}

// Runs on another CPU and calls back into the BSP, which is busy waiting for this very call.
void call_back_to_bsp(void*) noexcept {
  if (!ipi::call(0, &count_hit, nullptr).has_value()) {
    g_saw_stale_data.fetch_add(1, std::memory_order_relaxed);
  }
}

bool odd_cpus(const std::uint32_t cpu, const void*) noexcept { return (cpu & 1) != 0; }

void reset_counters() noexcept {
  for (auto& hits : g_hits) {
    hits.store(0, std::memory_order_relaxed);
  }

  g_async_sent.store(0, std::memory_order_relaxed);
  g_async_ran.store(0, std::memory_order_relaxed);
  g_saw_interrupts_on.store(0, std::memory_order_relaxed);
  g_saw_stale_data.store(0, std::memory_order_relaxed);
}

[[nodiscard]] std::uint64_t total_hits() noexcept {
  std::uint64_t sum = 0;
  for (const auto& hits : g_hits) {
    sum += hits.load(std::memory_order_relaxed);
  }
  return sum;
}

// Why the multi-CPU tests below cannot run here, or null if they can.
[[nodiscard]] const char* unsuitable(const std::uint32_t cpus) noexcept {
  if (cpus < 2) {
    return "needs a second CPU";
  }
  return cpus > TRACKED_CPUS ? "more CPUs than the counters cover" : nullptr;
}
}  // namespace

KTEST(Ipi, runs_a_call_on_the_cpu_it_names) {
  if (const char* why = unsuitable(smp::online_cpus())) {
    KTEST_SKIP(why);
  }

  for (std::uint32_t cpu = 0; cpu < smp::online_cpus(); ++cpu) {
    std::atomic<std::uint32_t> ran_on{smp::INVALID_CPU};
    KEXPECT(ipi::call(cpu, &record_cpu, &ran_on).has_value());
    KEXPECT_EQ(ran_on.load(), cpu);
  }

  KEXPECT(!ipi::call(smp::possible_cpus(), &record_cpu, nullptr).has_value());
}

KTEST(Ipi, reaches_every_other_cpu_or_all_of_them) {
  const std::uint32_t cpus = smp::online_cpus();
  if (const char* why = unsuitable(cpus)) {
    KTEST_SKIP(why);
  }

  reset_counters();
  for (std::uint64_t round = 1; round <= 100; ++round) {
    g_payload = 0xC0FFEE00 + round;
    const std::uint64_t expect = g_payload;
    KEXPECT_EQ(ipi::call_others(&check_payload, const_cast<std::uint64_t*>(&expect)), cpus - 1);
  }
  KEXPECT_EQ(g_saw_stale_data.load(), 0u);

  KEXPECT_EQ(ipi::call_others(&count_hit, nullptr), cpus - 1);
  KEXPECT_EQ(g_hits[hw::percpu::id()].load(), 0u);
  KEXPECT_EQ(total_hits(), cpus - 1);

  reset_counters();
  ipi::call_all(&count_hit, nullptr);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    KEXPECT_EQ(g_hits[cpu].load(), 1u);
  }

  reset_counters();
  KEXPECT_EQ(ipi::call_many(&odd_cpus, nullptr, &count_hit, nullptr), cpus / 2);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    KEXPECT_EQ(g_hits[cpu].load(), cpu & 1);
  }

  // Called functions run in interrupt context, with interrupts off.
  KEXPECT_EQ(g_saw_interrupts_on.load(), 0u);
}

KTEST(Ipi, handles_calls_back_into_a_waiting_caller) {
  const std::uint32_t cpus = smp::online_cpus();
  if (const char* why = unsuitable(cpus)) {
    KTEST_SKIP(why);
  }

  reset_counters();
  for (std::uint32_t cpu = 1; cpu < cpus; ++cpu) {
    KEXPECT(ipi::call(cpu, &call_back_to_bsp, nullptr).has_value());
  }

  KEXPECT_EQ(g_hits[0].load(), cpus - 1);
  KEXPECT_EQ(g_saw_stale_data.load(), 0u);
}

KTEST(Ipi, delivers_every_fire_and_forget_call) {
  constexpr std::uint32_t CALLS = 20'000;
  const std::uint32_t cpus = smp::online_cpus();
  if (const char* why = unsuitable(cpus)) {
    KTEST_SKIP(why);
  }

  reset_counters();
  Rng rng{42};
  for (std::uint32_t i = 0; i < CALLS; ++i) {
    const auto target = static_cast<std::uint32_t>(1 + rng.below(cpus - 1));
    KEXPECT(ipi::call(target, &count_async, nullptr, ipi::Wait::No).has_value());
  }

  // Inboxes are FIFO, so once this has run everywhere, so have the calls before it.
  (void)ipi::call_others(&count_async, nullptr);
  KEXPECT_EQ(g_async_ran.load(), CALLS + cpus - 1);
}

// Every CPU calls every other CPU, synchronously, while all the others do the same. Half the
// rounds run with interrupts disabled, so they only make progress by polling. Sprinkled in are
// fire-and-forget calls to random CPUs.
KTEST_ALL_CPUS(Ipi, survives_every_cpu_calling_every_other, cpu) {
  constexpr std::uint32_t ROUNDS = 2000;
  const std::uint32_t cpus = cpu_count();
  if (const char* why = unsuitable(cpus)) {
    KTEST_SKIP(why);
  }

  if (cpu == 0) {
    reset_counters();
  }
  sync_cpus();

  Rng rng{0x9E3779B97F4A7C15ull * (cpu + 1)};
  for (std::uint32_t round = 0; round < ROUNDS; ++round) {
    if (round & 1) {
      utils::IrqDisableGuard guard;
      (void)ipi::call_others(&count_hit, nullptr);
    } else {
      (void)ipi::call_others(&count_hit, nullptr);
    }

    if (round % 4 == 0) {
      g_async_sent.fetch_add(1, std::memory_order_relaxed);
      KEXPECT(ipi::call(static_cast<std::uint32_t>(rng.below(cpus)), &count_async, nullptr,
                        ipi::Wait::No)
                  .has_value());
    }
  }

  // Inboxes are FIFO, so once this has run everywhere, so have our earlier async calls.
  (void)ipi::call_others(&count_async, nullptr);
  g_async_sent.fetch_add(cpus - 1, std::memory_order_relaxed);
  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(total_hits(), std::uint64_t{ROUNDS} * cpus * (cpus - 1));
    KEXPECT_EQ(g_async_ran.load(), g_async_sent.load());
    KEXPECT_EQ(g_saw_interrupts_on.load(), 0u);
  }
}

// What a panic does: every other CPU parks for good. Runs last, as nothing can use them after.
KTEST_LAST(Smp, stops_every_other_cpu_like_a_panic) {
  const std::uint32_t cpus = smp::online_cpus();
  if (cpus < 2) {
    KTEST_SKIP("needs a second CPU to stop");
  }

  smp::stop_other_cpus();
  for (std::uint32_t cpu = 1; cpu < cpus; ++cpu) {
    KEXPECT(smp::entry(cpu).state.load() == smp::CpuState::Stopped);
  }
}
}  // namespace kernel::testing
