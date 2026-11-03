#include "ktest.hpp"

#include <atomic>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/io.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

extern "C" {
[[gnu::visibility("hidden")]] extern const std::uint8_t __ktest_cases_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __ktest_cases_end[];
}

namespace kernel::testing {
namespace {
using detail::TestCase;

// Test output goes straight to the console, without the logger's level and location prefix:
// every line already says where it comes from.
template <typename... Args>
void print(klib::format_string<Args...> fmt, Args&&... args) noexcept {
  char line[512];
  const std::size_t length = klib::kprint(line, sizeof(line), fmt, std::forward<Args>(args)...);
  utils::logger::write_console({line, std::min(length, sizeof(line) - 1)});
}

// Source paths relative to the tests directory, which is where nearly all failures are.
[[nodiscard]] std::string_view short_path(const std::string_view path) noexcept {
  if (const auto tests = path.rfind("/tests/"); tests != std::string_view::npos) {
    return path.substr(tests + 1);
  }

  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

constinit const TestCase* g_current = nullptr;
std::atomic<std::uint32_t> g_failures{0};
std::atomic<const char*> g_skip_reason{nullptr};
std::uint32_t g_participants = 1;

// A barrier for the CPUs of a multi-CPU test, which copes with CPUs that leave the test early.
class Barrier {
  utils::QSpinlock m_lock;
  std::uint32_t m_active = 0;
  std::uint32_t m_arrived = 0;
  std::atomic<std::uint64_t> m_generation{0};

  // Under m_lock: lets everybody waiting go on.
  void release() noexcept {
    m_arrived = 0;
    m_generation.fetch_add(1, std::memory_order_release);
  }

 public:
  void reset(const std::uint32_t participants) noexcept {
    utils::NakedGuard guard{m_lock};
    m_active = participants;
    m_arrived = 0;
  }

  void arrive() noexcept {
    std::uint64_t generation = 0;
    {
      utils::NakedGuard guard{m_lock};
      generation = m_generation.load(std::memory_order_relaxed);
      if (++m_arrived == m_active) {
        release();
        return;
      }
    }

    while (m_generation.load(std::memory_order_acquire) == generation) {
      hw::pause();
    }
  }

  // A CPU is done with the test: whoever waits for it no longer has to.
  void leave() noexcept {
    utils::NakedGuard guard{m_lock};
    --m_active;
    if (m_arrived != 0 && m_arrived == m_active) {
      release();
    }
  }
};

constinit Barrier g_barrier;

// Bumped by the BSP to start a multi-CPU test on every AP.
std::atomic<std::uint64_t> g_round{0};
std::atomic<std::uint32_t> g_finished_cpus{0};
std::atomic<bool> g_run_over{false};

void run_share(const std::uint32_t cpu) noexcept {
  g_current->body(cpu);
  g_barrier.leave();
  g_finished_cpus.fetch_add(1, std::memory_order_release);
}

void run_on_all_cpus() noexcept {
  g_participants = hw::smp::online_cpus();
  g_barrier.reset(g_participants);
  g_finished_cpus.store(0, std::memory_order_relaxed);
  g_round.fetch_add(1, std::memory_order_release);

  run_share(hw::percpu::id());

  while (g_finished_cpus.load(std::memory_order_acquire) != g_participants) {
    hw::pause();
  }

  g_participants = 1;
}

enum class Outcome : std::uint8_t { Passed, Failed, Skipped };

Outcome run_one(const TestCase& test) noexcept {
  g_current = &test;
  g_failures.store(0, std::memory_order_relaxed);
  g_skip_reason.store(nullptr, std::memory_order_relaxed);

  const klib::TimePoint start = drivers::SystemClock::now();
  if (test.mode == detail::Mode::AllCpus) {
    run_on_all_cpus();
  } else {
    test.body(hw::percpu::id());
  }

  const std::int64_t ms = (drivers::SystemClock::now() - start).to_ms();

  const std::uint32_t failures = g_failures.load(std::memory_order_acquire);
  const char* skipped = g_skip_reason.load(std::memory_order_acquire);
  g_current = nullptr;

  if (failures != 0) {
    print("ktest: FAIL {}.{} ({} ms)\n", test.suite, test.name, ms);
    return Outcome::Failed;
  }

  if (skipped != nullptr) {
    print("ktest: skip {}.{}: {}\n", test.suite, test.name, skipped);
    return Outcome::Skipped;
  }

  print("ktest: pass {}.{} ({} ms)\n", test.suite, test.name, ms);
  return Outcome::Passed;
}

std::span<const TestCase> registered_tests() noexcept {
  const std::size_t bytes = reinterpret_cast<std::uintptr_t>(__ktest_cases_end) -
                            reinterpret_cast<std::uintptr_t>(__ktest_cases_start);
  return {reinterpret_cast<const TestCase*>(__ktest_cases_start), bytes / sizeof(TestCase)};
}

// Ends QEMU when it has an isa-debug-exit device at the port: it exits with (code << 1) | 1.
// Elsewhere nothing listens there, and the write goes nowhere.
void report_to_emulator([[maybe_unused]] const bool passed) noexcept {
#ifdef EREBUS_KTEST_EXIT_PORT
  hw::IoResource{hw::IoType::PMIO, EREBUS_KTEST_EXIT_PORT}.write<std::uint32_t>(0, passed ? 0 : 1);
#endif
}
}  // namespace

void run_all() noexcept {
  const std::span<const TestCase> tests = registered_tests();
  print("ktest: running {} tests on {} CPUs\n", tests.size(), hw::smp::online_cpus());

  std::uint32_t passed = 0;
  std::uint32_t failed = 0;
  std::uint32_t skipped = 0;

  for (const detail::Order order : {detail::Order::Normal, detail::Order::Last}) {
    for (const TestCase& test : tests) {
      if (test.order != order) {
        continue;
      }

      switch (run_one(test)) {
        case Outcome::Passed:
          ++passed;
          break;
        case Outcome::Failed:
          ++failed;
          break;
        case Outcome::Skipped:
          ++skipped;
          break;
      }
    }
  }

  g_run_over.store(true, std::memory_order_release);

  print("ktest: {} passed, {} failed, {} skipped\n", passed, failed, skipped);
  if (failed == 0) {
    print("ktest: ALL PASSED\n");
  } else {
    print("ktest: FAILED\n");
  }
  report_to_emulator(failed == 0);
}

void run_on_ap() noexcept {
  const std::uint32_t cpu = hw::percpu::id();
  std::uint64_t seen = 0;

  while (!g_run_over.load(std::memory_order_acquire)) {
    const std::uint64_t round = g_round.load(std::memory_order_acquire);
    if (round == seen) {
      hw::pause();
      continue;
    }

    seen = round;
    run_share(cpu);
  }
}

std::uint32_t cpu_count() noexcept { return g_participants; }

void sync_cpus() noexcept {
  if (g_participants > 1) {
    g_barrier.arrive();
  }
}

namespace detail {
void fail(const std::source_location where, const std::string_view message) noexcept {
  g_failures.fetch_add(1, std::memory_order_relaxed);

  const std::string_view suite = g_current != nullptr ? g_current->suite : "?";
  const std::string_view name = g_current != nullptr ? g_current->name : "?";
  const std::string_view file = short_path(where.file_name());
  if (g_participants > 1) {
    print("ktest:   {}.{}: {}:{} on CPU {}: {}\n", suite, name, file, where.line(),
          hw::percpu::id(), message);
  } else {
    print("ktest:   {}.{}: {}:{}: {}\n", suite, name, file, where.line(), message);
  }
}

void skip(const char* reason) noexcept { g_skip_reason.store(reason, std::memory_order_release); }

bool compare_strings(const std::string_view a, const std::string_view b, const char* expr_a,
                     const char* expr_b, const std::source_location where) noexcept {
  if (a == b) {
    return true;
  }

  fail_with(where, "expected {} == {}, but they are \"{}\" and \"{}\"", expr_a, expr_b, a, b);
  return false;
}

bool compare_byte_spans(const std::span<const std::byte> a, const std::span<const std::byte> b,
                        const char* expr_a, const char* expr_b,
                        const std::source_location where) noexcept {
  const std::size_t common = std::min(a.size(), b.size());
  std::size_t at = 0;
  while (at < common && a[at] == b[at]) {
    ++at;
  }

  if (at == common && a.size() == b.size()) {
    return true;
  }

  if (at == common) {
    fail_with(where, "expected {} and {} to be equal, but they are {} and {} bytes long", expr_a,
              expr_b, a.size(), b.size());
  } else {
    fail_with(where, "expected {} and {} to be equal, but byte {} is {:#04x} and {:#04x}", expr_a,
              expr_b, at, std::to_integer<unsigned>(a[at]), std::to_integer<unsigned>(b[at]));
  }

  return false;
}
}  // namespace detail
}  // namespace kernel::testing
