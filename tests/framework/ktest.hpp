/// @file ktest.hpp
/// @brief The in-kernel test framework.
///
/// The tests in tests/ are built into kernel_test, a second kernel image that boots exactly like
/// the real one and then, instead of going idle, runs every registered test, one after another.
/// Results go to the serial console:
///
///     ktest: running 214 tests on 4 CPUs
///     ktest: pass Maths.align_up_rounds_up (0 ms)
///     ktest: FAIL Blake2b.matches_rfc7693 (0 ms)
///     ktest: 213 passed, 1 failed, 0 skipped
///     ktest: FAILED
///
/// Run under QEMU with an isa-debug-exit device, the image then ends QEMU with exit status 1 if
/// everything passed and 3 if not.
///
/// @code
/// KTEST(Maths, align_up_rounds_up) {
///   KEXPECT_EQ(utils::maths::align_up(0x1234u, 0x1000u), 0x2000u);
/// }
///
/// // The body runs on every online CPU at once; `cpu` is the dense ID of the one running it.
/// KTEST_ALL_CPUS(QSpinlock, keeps_a_counter_exact, cpu) {
///   hammer(g_lock, g_counter);
///   testing::sync_cpus();  // nobody goes on until every CPU got here
///   KEXPECT_EQ(g_counter, ROUNDS * testing::cpu_count());
/// }
/// @endcode
///
/// KEXPECT_*() report a failure and let the test carry on. KREQUIRE_*() report it and return
/// from the test body, so they belong in the body itself, not in helpers or lambdas.
/// KTEST_SKIP() ends a test that cannot run on this machine, such as a two-CPU test on one.
///
/// Tests run in the order the linker placed them, file by file, except that KTEST_LAST() tests
/// come after all the others: they are for tests that leave the machine changed for good.
/// Every test runs in thread context with interrupts enabled; a failed KASSERT, a fault or a
/// hang stops the whole run, which the scripts that boot the image report as a failure.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <kformat.hpp>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#define KTEST_CONCAT_INNER(a, b) a##b
#define KTEST_CONCAT(a, b) KTEST_CONCAT_INNER(a, b)

// The registration of one test: its body, and an entry in the ktest_cases section that the
// runner walks. The section is named like a C identifier so that lld never folds two entries.
#define KTEST_REGISTER(suite, name, mode, order, cpu)                                          \
  static void KTEST_CONCAT(ktest_body_, __LINE__)(std::uint32_t cpu) noexcept;                 \
  [[gnu::used,                                                                                 \
    gnu::section("ktest_cases")]] static constinit const ::kernel::testing::detail::TestCase   \
      KTEST_CONCAT(ktest_case_, __LINE__){#suite, #name, &KTEST_CONCAT(ktest_body_, __LINE__), \
                                          ::kernel::testing::detail::Mode::mode,               \
                                          ::kernel::testing::detail::Order::order};            \
  static void KTEST_CONCAT(ktest_body_, __LINE__)([[maybe_unused]] std::uint32_t cpu) noexcept

/// @brief Defines a test that runs on the BSP while the other CPUs wait.
#define KTEST(suite, name) KTEST_REGISTER(suite, name, Bsp, Normal, ktest_cpu)

/// @brief Defines a test whose body runs on every online CPU at the same time. `cpu` names the
///        parameter that holds the dense ID of the CPU running it; the BSP is 0.
#define KTEST_ALL_CPUS(suite, name, cpu) KTEST_REGISTER(suite, name, AllCpus, Normal, cpu)

/// @brief A KTEST() that runs after every other test, for one that leaves the machine changed.
#define KTEST_LAST(suite, name) KTEST_REGISTER(suite, name, Bsp, Last, ktest_cpu)

#define KTEST_HERE std::source_location::current()

/// @brief Fails the test if `cond` is false.
#define KEXPECT(cond) ::kernel::testing::detail::check(static_cast<bool>(cond), #cond, KTEST_HERE)

#define KEXPECT_EQ(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Eq>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_NE(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Ne>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_LT(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Lt>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_LE(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Le>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_GT(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Gt>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_GE(a, b) \
  ::kernel::testing::detail::compare<::kernel::testing::detail::Op::Ge>(a, b, #a, #b, KTEST_HERE)

/// @brief Fails the test unless the two strings are equal. Null pointers count as "(null)".
#define KEXPECT_STREQ(a, b) ::kernel::testing::detail::compare_strings(a, b, #a, #b, KTEST_HERE)

/// @brief Fails the test unless the two byte ranges are equal, and says where they first
///        differ.
#define KEXPECT_BYTES_EQ(a, b) ::kernel::testing::detail::compare_bytes(a, b, #a, #b, KTEST_HERE)

// KREQUIRE_*: the same, then out of the test body.
#define KTEST_OR_RETURN(check) \
  do {                         \
    if (!(check)) {            \
      return;                  \
    }                          \
  } while (false)

// Tests the condition itself, so that the early return is plain to see for static analysis:
// `KREQUIRE(r.has_value());` makes the `*r` that follows provably safe.
#define KREQUIRE(cond)                                            \
  do {                                                            \
    if (!static_cast<bool>(cond)) {                               \
      ::kernel::testing::detail::check(false, #cond, KTEST_HERE); \
      return;                                                     \
    }                                                             \
  } while (false)
#define KREQUIRE_EQ(a, b) KTEST_OR_RETURN(KEXPECT_EQ(a, b))
#define KREQUIRE_NE(a, b) KTEST_OR_RETURN(KEXPECT_NE(a, b))
#define KREQUIRE_LT(a, b) KTEST_OR_RETURN(KEXPECT_LT(a, b))
#define KREQUIRE_LE(a, b) KTEST_OR_RETURN(KEXPECT_LE(a, b))
#define KREQUIRE_GT(a, b) KTEST_OR_RETURN(KEXPECT_GT(a, b))
#define KREQUIRE_GE(a, b) KTEST_OR_RETURN(KEXPECT_GE(a, b))
#define KREQUIRE_STREQ(a, b) KTEST_OR_RETURN(KEXPECT_STREQ(a, b))
#define KREQUIRE_BYTES_EQ(a, b) KTEST_OR_RETURN(KEXPECT_BYTES_EQ(a, b))

/// @brief Ends the test as skipped, saying why. Only in the test body itself.
#define KTEST_SKIP(reason)                   \
  do {                                       \
    ::kernel::testing::detail::skip(reason); \
    return;                                  \
  } while (false)

namespace kernel::testing {
/// @brief Runs every registered test and reports the results. Called once, by the BSP, once
///        every CPU is online. Returns when the run is over; the other CPUs go idle then.
void run_all() noexcept;

/// @brief Where the APs wait for multi-CPU tests; install with hw::smp::set_ap_main().
void run_on_ap() noexcept;

/// @brief How many CPUs run the current test: every online one for KTEST_ALL_CPUS(), else 1.
[[nodiscard]] std::uint32_t cpu_count() noexcept;

/// @brief Waits until every CPU running the current test has called it, as often as this one.
///
/// A CPU that leaves the test early, through KREQUIRE_*() say, no longer counts, so the others
/// do not wait for it forever. Does nothing in a single-CPU test.
void sync_cpus() noexcept;

/// @brief A small, fast, deterministic generator (xorshift64), for test data. Not random enough
///        for anything else.
class Rng {
  std::uint64_t m_state;

 public:
  constexpr explicit Rng(const std::uint64_t seed) noexcept
      : m_state(seed != 0 ? seed : 0x9E37'79B9'7F4A'7C15ull) {}

  constexpr std::uint64_t next() noexcept {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 7;
    m_state ^= m_state << 17;
    return m_state;
  }

  /// @brief A value in [0, `bound`), very slightly biased. `bound` must not be 0.
  constexpr std::uint64_t below(const std::uint64_t bound) noexcept { return next() % bound; }
};

// The checks return whether they held, which KREQUIRE_*() use, and KEXPECT_*() ignore.
namespace detail {
enum class Mode : std::uint8_t { Bsp, AllCpus };
enum class Order : std::uint8_t { Normal, Last };

struct TestCase {
  const char* suite;
  const char* name;
  void (*body)(std::uint32_t cpu) noexcept;
  Mode mode;
  Order order;
};

enum class Op : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

/// @brief Counts a failure of the current test and prints `message` with where it happened.
void fail(std::source_location where, std::string_view message) noexcept;

/// @brief Marks the current test skipped, for `reason`.
void skip(const char* reason) noexcept;

/// @brief Formats a failure message into a buffer of its own, then fail()s with it.
template <typename... Args>
void fail_with(const std::source_location where, klib::format_string<Args...> fmt,
               Args&&... args) noexcept {
  char text[384];
  klib::kprint(text, sizeof(text), fmt, std::forward<Args>(args)...);
  fail(where, text);
}

inline bool check(const bool ok, const char* expr, const std::source_location where) noexcept {
  if (!ok) [[unlikely]] {
    fail_with(where, "expected {}", expr);
  }

  return ok;
}

// Integers of any signedness compare by value, as std::cmp_equal() and friends do; characters
// and bool are left to the operators, which std::cmp_*() refuse.
template <typename T>
concept PlainInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                       !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
                       !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

template <Op op, typename A, typename B>
[[nodiscard]] constexpr bool holds(const A& a, const B& b) noexcept {
  using PlainA = std::remove_cv_t<A>;
  using PlainB = std::remove_cv_t<B>;
  if constexpr (PlainInteger<PlainA> && PlainInteger<PlainB>) {
    // Read once, as the unqualified type: the value may be volatile.
    const PlainA x = a;
    const PlainB y = b;
    if constexpr (op == Op::Eq) return std::cmp_equal(x, y);
    if constexpr (op == Op::Ne) return std::cmp_not_equal(x, y);
    if constexpr (op == Op::Lt) return std::cmp_less(x, y);
    if constexpr (op == Op::Le) return std::cmp_less_equal(x, y);
    if constexpr (op == Op::Gt) return std::cmp_greater(x, y);
    if constexpr (op == Op::Ge) return std::cmp_greater_equal(x, y);
  } else {
    if constexpr (op == Op::Eq) return a == b;
    if constexpr (op == Op::Ne) return a != b;
    if constexpr (op == Op::Lt) return a < b;
    if constexpr (op == Op::Le) return a <= b;
    if constexpr (op == Op::Gt) return a > b;
    if constexpr (op == Op::Ge) return a >= b;
  }
}

[[nodiscard]] constexpr std::string_view spelling(const Op op) noexcept {
  constexpr std::string_view names[] = {"==", "!=", "<", "<=", ">", ">="};
  return names[std::to_underlying(op)];
}

template <Op op, typename A, typename B>
bool compare(const A& a, const B& b, const char* expr_a, const char* expr_b,
             const std::source_location where) noexcept {
  const bool ok = holds<op>(a, b);
  if (!ok) [[unlikely]] {
    fail_with(where, "expected {} {} {}, but {} is {} and {} is {}", expr_a, spelling(op), expr_b,
              expr_a, a, expr_b, b);
  }

  return ok;
}

bool compare_strings(std::string_view a, std::string_view b, const char* expr_a, const char* expr_b,
                     std::source_location where) noexcept;

inline bool compare_strings(const char* a, const char* b, const char* expr_a, const char* expr_b,
                            const std::source_location where) noexcept {
  return compare_strings(a ? std::string_view{a} : "(null)", b ? std::string_view{b} : "(null)",
                         expr_a, expr_b, where);
}

bool compare_byte_spans(std::span<const std::byte> a, std::span<const std::byte> b,
                        const char* expr_a, const char* expr_b,
                        std::source_location where) noexcept;

/// @brief Compares the bytes of two spans of anything trivially copyable, of any extent.
template <typename T, std::size_t N, typename U, std::size_t M>
  requires std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<U>
bool compare_bytes(const std::span<T, N> a, const std::span<U, M> b, const char* expr_a,
                   const char* expr_b, const std::source_location where) noexcept {
  return compare_byte_spans(std::as_bytes(a), std::as_bytes(b), expr_a, expr_b, where);
}
}  // namespace detail
}  // namespace kernel::testing
