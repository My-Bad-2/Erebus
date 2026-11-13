#include <stdlib.h>

#include <limits>

#include "ktest.hpp"

namespace ktest {
KTEST(Abs, returns_the_magnitude) {
  KEXPECT_EQ(klib::abs(-42), 42);
  KEXPECT_EQ(klib::abs(42), 42);
  KEXPECT_EQ(klib::abs(0), 0);
  KEXPECT_EQ(klib::labs(-99999L), 99999L);
  KEXPECT_EQ(klib::llabs(-9999999999999LL), 9999999999999LL);
  KEXPECT_EQ(klib::imaxabs(-1234567890), 1234567890);
}

// The most negative value has no positive counterpart; it comes back as it went in.
KTEST(Abs, leaves_the_most_negative_value_alone) {
  KEXPECT_EQ(klib::abs(std::numeric_limits<int>::min()), std::numeric_limits<int>::min());
  KEXPECT_EQ(klib::llabs(std::numeric_limits<long long>::min()),
             std::numeric_limits<long long>::min());
}

KTEST(Div, rounds_towards_zero) {
  const auto d = klib::div(38, 5);
  KEXPECT_EQ(d.quot, 7);
  KEXPECT_EQ(d.rem, 3);

  const auto ld = klib::ldiv(1000L, 11L);
  KEXPECT_EQ(ld.quot, 90L);
  KEXPECT_EQ(ld.rem, 10L);

  const auto lld = klib::lldiv(1000000000000LL, 3LL);
  KEXPECT_EQ(lld.quot, 333333333333LL);
  KEXPECT_EQ(lld.rem, 1LL);

  // The remainder takes the sign of the dividend.
  const auto neg_num = klib::div(-10, 3);
  KEXPECT_EQ(neg_num.quot, -3);
  KEXPECT_EQ(neg_num.rem, -1);
  const auto neg_den = klib::div(10, -3);
  KEXPECT_EQ(neg_den.quot, -3);
  KEXPECT_EQ(neg_den.rem, 1);
  const auto both = klib::div(-10, -3);
  KEXPECT_EQ(both.quot, 3);
  KEXPECT_EQ(both.rem, -1);

  const auto imax = klib::imaxdiv(-7, 2);
  KEXPECT_EQ(imax.quot, -3);
  KEXPECT_EQ(imax.rem, -1);
}
}  // namespace ktest
