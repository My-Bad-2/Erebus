// The part of the runner that is the same wherever it runs: walking the registered tests, keeping
// count, and the checks. platform.hpp says what each place adds.
#include "ktest.hpp"

#include <algorithm>
#include <atomic>

#include "platform.hpp"

extern "C" {
[[gnu::visibility("hidden")]] extern const std::uint8_t __ktest_cases_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __ktest_cases_end[];
}

namespace ktest {
namespace {
using detail::TestCase;

// Source paths relative to the tests directory, which is where nearly all failures are.
[[nodiscard]] std::string_view short_path(const std::string_view path) noexcept {
  if (const auto tests = path.rfind("/tests/"); tests != std::string_view::npos) {
    return path.substr(tests + 1);
  }

  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The test that runs, and what it came to so far. Participants of a multi-CPU test all write the
// last two.
constinit const TestCase* g_current = nullptr;
constinit std::atomic<std::uint32_t> g_failures{0};
constinit std::atomic<const char*> g_skip_reason{nullptr};

enum class Outcome : std::uint8_t { Passed, Failed, Skipped };

Outcome run(const TestCase& test) noexcept {
  g_current = &test;
  g_failures.store(0, std::memory_order_relaxed);
  g_skip_reason.store(nullptr, std::memory_order_relaxed);

  const std::uint64_t start = platform::now_ns();
  if (test.mode == detail::Mode::AllCpus) {
    platform::run_everywhere(test);
  } else {
    test.body(0);
  }

  const std::uint64_t ms = (platform::now_ns() - start) / 1'000'000;

  const std::uint32_t failures = g_failures.load(std::memory_order_acquire);
  const char* skipped = g_skip_reason.load(std::memory_order_acquire);
  g_current = nullptr;

  if (failures != 0) {
    detail::print("FAIL {}.{} ({} ms)\n", test.suite, test.name, ms);
    return Outcome::Failed;
  }

  if (skipped != nullptr) {
    detail::print("skip {}.{}: {}\n", test.suite, test.name, skipped);
    return Outcome::Skipped;
  }

  detail::print("pass {}.{} ({} ms)\n", test.suite, test.name, ms);
  return Outcome::Passed;
}
}  // namespace

namespace detail {
std::span<const TestCase> registered_tests() noexcept {
  const std::size_t bytes = reinterpret_cast<std::uintptr_t>(__ktest_cases_end) -
                            reinterpret_cast<std::uintptr_t>(__ktest_cases_start);
  return {reinterpret_cast<const TestCase*>(__ktest_cases_start), bytes / sizeof(TestCase)};
}

Totals run_one(const TestCase& test) noexcept {
  switch (run(test)) {
    case Outcome::Passed:
      return Totals{.passed = 1};
    case Outcome::Failed:
      return Totals{.failed = 1};
    case Outcome::Skipped:
      return Totals{.skipped = 1};
  }

  return {};
}

Totals run_tests(const Order order) noexcept {
  Totals totals;
  for (const TestCase& test : registered_tests()) {
    if (test.order == order) {
      totals += run_one(test);
    }
  }

  return totals;
}

void report(const Totals& totals) noexcept {
  print("{} passed, {} failed, {} skipped\n", totals.passed, totals.failed, totals.skipped);
  if (totals.failed == 0) {
    print("ALL PASSED\n");
  } else {
    print("FAILED\n");
  }
}

void fail(const std::source_location where, const std::string_view message) noexcept {
  g_failures.fetch_add(1, std::memory_order_relaxed);

  const std::string_view suite = g_current != nullptr ? g_current->suite : "?";
  const std::string_view name = g_current != nullptr ? g_current->name : "?";
  const std::string_view file = short_path(where.file_name());
  if (cpu_count() > 1) {
    print("  {}.{}: {}:{} on {} {}: {}\n", suite, name, file, where.line(),
          platform::participant_kind(), platform::participant(), message);
  } else {
    print("  {}.{}: {}:{}: {}\n", suite, name, file, where.line(), message);
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
}  // namespace ktest
