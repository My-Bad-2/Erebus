#include "chrono.hpp"

#include <cstdint>
#include <limits>

#include "ktest.hpp"

using namespace klib::literals;

KTEST(Duration, units_and_conversions) {
  KEXPECT_EQ((1_s).to_ns(), 1'000'000'000);
  KEXPECT_EQ((3_ms).to_us(), 3'000);
  KEXPECT_EQ((1500_us).to_ms(), 1);
  KEXPECT_EQ((2'999_ms).to_secs(), 2);
  KEXPECT_EQ(klib::Duration::microseconds(-1500).to_ms(), -1);
}

KTEST(Duration, arithmetic) {
  klib::Duration d = 1_s;
  d += 500_ms;
  d -= 250_ms;
  KEXPECT_EQ(d.to_ms(), 1250);
  KEXPECT_EQ((d * 2).to_ms(), 2500);
  KEXPECT_EQ((d / 5).to_ms(), 250);
  KEXPECT_EQ((-d).to_ms(), -1250);
  KEXPECT_LT(1_ms, 1_s);
  KEXPECT_EQ(1000_us, 1_ms);
}

KTEST(TimePoint, add_and_subtract) {
  const klib::TimePoint start{1'000};
  const klib::TimePoint later = start + 5_us;
  KEXPECT_EQ(later.raw_ns(), 6'000u);
  KEXPECT_EQ((later - start).to_ns(), 5'000);
  KEXPECT_EQ((start - later).to_ns(), -5'000);
  KEXPECT_EQ((later - 5_us), start);
  KEXPECT_LT(start, later);
}

KTEST(TimePoint, difference_survives_wrap_around) {
  const klib::TimePoint before{std::numeric_limits<std::uint64_t>::max() - 10};
  const klib::TimePoint after = before + 20_ns;
  KEXPECT_EQ(after.raw_ns(), 9u);
  KEXPECT_EQ((after - before).to_ns(), 20);
}
