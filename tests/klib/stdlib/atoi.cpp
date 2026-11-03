#include <stdlib.h>

#include <limits>

#include "ktest.hpp"

namespace kernel::testing {
KTEST(Atoi, parses_decimal_numbers) {
  KEXPECT_EQ(klib::atoi("0"), 0);
  KEXPECT_EQ(klib::atoi("42"), 42);
  KEXPECT_EQ(klib::atoi("-42"), -42);
  KEXPECT_EQ(klib::atoi("+7"), 7);
  KEXPECT_EQ(klib::atoi("2147483647"), 2147483647);
  KEXPECT_EQ(klib::atol("-123456789"), -123456789L);
  KEXPECT_EQ(klib::atoll("9876543210"), 9876543210LL);
}

KTEST(Atoi, skips_leading_whitespace_only) {
  KEXPECT_EQ(klib::atoi("   12"), 12);
  KEXPECT_EQ(klib::atoi("\t\n\v\f\r 34"), 34);
  KEXPECT_EQ(klib::atoi(" - 5"), 0);
  KEXPECT_EQ(klib::atoi("--5"), 0);
  KEXPECT_EQ(klib::atoi("+-5"), 0);
}

KTEST(Atoi, stops_at_the_first_non_digit) {
  KEXPECT_EQ(klib::atoi("123abc"), 123);
  KEXPECT_EQ(klib::atoi("12 34"), 12);
  KEXPECT_EQ(klib::atoi("0x1F"), 0);
  KEXPECT_EQ(klib::atoi("abc"), 0);
  KEXPECT_EQ(klib::atoi(""), 0);
  KEXPECT_EQ(klib::atoi("3.99"), 3);
}

KTEST(Atoll, reaches_both_limits) {
  KEXPECT_EQ(klib::atoll("9223372036854775807"), std::numeric_limits<long long>::max());
  KEXPECT_EQ(klib::atoll("-9223372036854775808"), std::numeric_limits<long long>::min());
}
}  // namespace kernel::testing
