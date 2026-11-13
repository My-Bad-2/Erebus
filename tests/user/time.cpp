// The Time family from user code.
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
constexpr std::uint64_t MILLISECOND = 1'000'000;
}  // namespace

KTEST(Time, the_clock_only_goes_forward) {
  std::uint64_t previous = sys::time::now();
  KEXPECT_GT(previous, 0u);
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t now = sys::time::now();
    KREQUIRE_GE(now, previous);
    previous = now;
  }
}

KTEST(Time, sleep_for_takes_at_least_as_long_as_asked) {
  const std::uint64_t start = sys::time::now();
  KEXPECT_OK(sys::time::sleep_for(3 * MILLISECOND));
  KEXPECT_GE(sys::time::now() - start, 3 * MILLISECOND);
}

KTEST(Time, sleep_until_waits_for_the_deadline) {
  const std::uint64_t deadline = sys::time::now() + 2 * MILLISECOND;
  KEXPECT_OK(sys::time::sleep_until(deadline));
  KEXPECT_GE(sys::time::now(), deadline);

  // One that has passed already is no wait at all.
  KEXPECT_OK(sys::time::sleep_until(0));
}

KTEST(Time, sleep_refuses_flags_it_does_not_know) {
  KEXPECT_ERROR(sys::time::sleep(MILLISECOND, 0, static_cast<sys::SleepFlags>(2)),
                sys::Error::InvalidFlags);
}
}  // namespace user::testing
