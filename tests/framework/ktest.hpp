/// @file ktest.hpp
/// @brief The test framework, in the kernel and in user space alike.
///
/// Tests are built into two test images, each of which boots like the real thing and runs its
/// tests instead of carrying on:
///
///  - kernel_test, the kernel with tests/kernel/ and tests/klib/ built in, which runs them on the
///    BSP and, for KTEST_ALL_CPUS(), on every CPU at once;
///  - user_test, the root task with tests/user/ and tests/klib/ built in, which runs them in ring 3
///    and, for KTEST_ALL_CPUS(), on several threads at once (see USER_PARTICIPANTS).
///
/// So klib's tests run twice, once against klib as the kernel builds it, and once against the
/// copy user code gets, which is compiled for a different ABI. The test kernel starts the user
/// image as its root task once its own tests are through, waits for it, and counts its exit status
/// as one more result. Results go to the serial console, the user image's through DebugLog:
///
///     ktest: running 665 tests on 4 CPUs
///     ktest: pass Maths.align_up_rounds_up (0 ms)
///     ...
///     [INF] [root_task:50]      | utest: running 190 tests
///     [INF] [root_task:50]      | utest: pass Endpoint.six_words_go_across_in_registers (0 ms)
///     [INF] [root_task:50]      | utest: 190 passed, 0 failed, 0 skipped
///     ktest: pass UserSpace.every_user_test_passes (402 ms)
///     ktest: 658 passed, 0 failed, 7 skipped
///     ktest: ALL PASSED
///
/// Run under QEMU with an isa-debug-exit device, the test kernel then ends QEMU with exit status 1
/// if everything passed and 3 if not.
///
/// @code
/// KTEST(Maths, align_up_rounds_up) {
///   KEXPECT_EQ(utils::maths::align_up(0x1234u, 0x1000u), 0x2000u);
/// }
///
/// // The body runs on every participant at once; `cpu` is the one running it, the BSP or the
/// // first thread being 0.
/// KTEST_ALL_CPUS(QSpinlock, keeps_a_counter_exact, cpu) {
///   hammer(g_lock, g_counter);
///   ktest::sync_cpus();  // nobody goes on until every participant got here
///   KEXPECT_EQ(g_counter, ROUNDS * ktest::cpu_count());
/// }
/// @endcode
///
/// KEXPECT_*() report a failure and let the test carry on. KREQUIRE_*() report it and return
/// from the test body, so they belong in the body itself, not in helpers or lambdas.
/// KTEST_SKIP() ends a test that cannot run on this machine, such as a two-CPU test on one.
///
/// Tests run in the order the linker placed them, file by file, except that KTEST_LAST() tests
/// come after all the others, the user image included: they are for tests that leave the machine
/// changed for good. Every test runs in thread context with interrupts enabled; in the kernel, a
/// failed KASSERT, a fault or a hang stops the whole run, which the scripts that boot the image
/// report as a failure, and in user space, a fault ends the image, which fails the run the same
/// way.
///
/// The kernel's tests live in namespace kernel::testing and the user image's in user::testing,
/// each of which has the framework's names in it; klib's, which both build, in ktest itself.
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
  [[gnu::used, gnu::section("ktest_cases")]] static constinit const ::ktest::detail::TestCase  \
      KTEST_CONCAT(ktest_case_, __LINE__){#suite, #name, &KTEST_CONCAT(ktest_body_, __LINE__), \
                                          ::ktest::detail::Mode::mode,                         \
                                          ::ktest::detail::Order::order};                      \
  static void KTEST_CONCAT(ktest_body_, __LINE__)([[maybe_unused]] std::uint32_t cpu) noexcept

/// @brief Defines a test that runs on one CPU or thread while the others wait.
#define KTEST(suite, name) KTEST_REGISTER(suite, name, Bsp, Normal, ktest_cpu)

/// @brief Defines a test whose body runs on every participant at the same time. `cpu` names the
///        parameter that holds which one is running it; the BSP, or the first thread, is 0.
#define KTEST_ALL_CPUS(suite, name, cpu) KTEST_REGISTER(suite, name, AllCpus, Normal, cpu)

/// @brief A KTEST() that runs after every other test, for one that leaves the machine changed.
#define KTEST_LAST(suite, name) KTEST_REGISTER(suite, name, Bsp, Last, ktest_cpu)

#define KTEST_HERE std::source_location::current()

/// @brief Fails the test if `cond` is false.
#define KEXPECT(cond) ::ktest::detail::check(static_cast<bool>(cond), #cond, KTEST_HERE)

#define KEXPECT_EQ(a, b) ::ktest::detail::compare<::ktest::detail::Op::Eq>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_NE(a, b) ::ktest::detail::compare<::ktest::detail::Op::Ne>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_LT(a, b) ::ktest::detail::compare<::ktest::detail::Op::Lt>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_LE(a, b) ::ktest::detail::compare<::ktest::detail::Op::Le>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_GT(a, b) ::ktest::detail::compare<::ktest::detail::Op::Gt>(a, b, #a, #b, KTEST_HERE)
#define KEXPECT_GE(a, b) ::ktest::detail::compare<::ktest::detail::Op::Ge>(a, b, #a, #b, KTEST_HERE)

/// @brief Fails the test unless the two strings are equal. Null pointers count as "(null)".
#define KEXPECT_STREQ(a, b) ::ktest::detail::compare_strings(a, b, #a, #b, KTEST_HERE)

/// @brief Fails the test unless the two byte ranges are equal, and says where they first
///        differ.
#define KEXPECT_BYTES_EQ(a, b) ::ktest::detail::compare_bytes(a, b, #a, #b, KTEST_HERE)

// KREQUIRE_*: the same, then out of the test body.
#define KTEST_OR_RETURN(check) \
  do {                         \
    if (!(check)) {            \
      return;                  \
    }                          \
  } while (false)

// Tests the condition itself, so that the early return is plain to see for static analysis:
// `KREQUIRE(r.has_value());` makes the `*r` that follows provably safe.
#define KREQUIRE(cond)                                  \
  do {                                                  \
    if (!static_cast<bool>(cond)) {                     \
      ::ktest::detail::check(false, #cond, KTEST_HERE); \
      return;                                           \
    }                                                   \
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
#define KTEST_SKIP(reason)         \
  do {                             \
    ::ktest::detail::skip(reason); \
    return;                        \
  } while (false)

namespace ktest {
/// @brief How many threads a KTEST_ALL_CPUS() body runs on in user space, which has no way to ask
///        how many CPUs there are. More than one, so that the body does run concurrently, and
///        enough that two CPUs or more get some of them.
inline constexpr std::uint32_t USER_PARTICIPANTS = 4;

/// @brief What a run of tests came to.
struct Totals {
  std::uint32_t passed{0};
  std::uint32_t failed{0};
  std::uint32_t skipped{0};

  constexpr Totals& operator+=(const Totals& other) noexcept {
    passed += other.passed;
    failed += other.failed;
    skipped += other.skipped;
    return *this;
  }
};

/// @brief How many participants run the current test: all of them for KTEST_ALL_CPUS(), else 1.
[[nodiscard]] std::uint32_t cpu_count() noexcept;

/// @brief Waits until every participant in the current test has called it, as often as this one.
///
/// A participant that leaves the test early, through KREQUIRE_*() say, no longer counts, so the
/// others do not wait for it forever. Does nothing in a test that runs on one.
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

#ifdef EREBUS_USER_TESTS
/// @brief Runs every test in the user test image and reports the results.
/// @return The image's exit status: 0 if none failed, 1 if any did.
[[nodiscard]] int run_all() noexcept;
#endif
}  // namespace ktest

#ifdef EREBUS_USER_TESTS
// Where the user image's tests live, with the framework's names in reach.
namespace user::testing {
using ::ktest::cpu_count;
using ::ktest::Rng;
using ::ktest::sync_cpus;
}  // namespace user::testing
#endif

#ifdef EREBUS_KERNEL_TESTS
namespace kernel::proc {
class Process;
}  // namespace kernel::proc

// Where the kernel's tests live, with the framework's names in reach.
namespace kernel::testing {
using ::ktest::cpu_count;
using ::ktest::Rng;
using ::ktest::sync_cpus;

/// @brief Starts the user test image, or says there is none with null. The process it returns
///        is a child of process 0, which run_all() collects.
using StartUserTests = proc::Process* (*)() noexcept;

/// @brief Runs every registered test and reports the results: the kernel's own, then the user
///        test image @p start_user_tests starts, then the KTEST_LAST() ones. Called once, by the
///        BSP, once every CPU is online. Returns when the run is over; the other CPUs go idle then.
void run_all(StartUserTests start_user_tests) noexcept;

/// @brief Where the APs wait for multi-CPU tests; install with hw::smp::set_ap_main().
void run_on_ap() noexcept;
}  // namespace kernel::testing
#endif
