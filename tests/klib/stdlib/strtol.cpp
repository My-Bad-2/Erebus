#include <stdlib.h>

#include <limits>

#include "ktest.hpp"

namespace ktest {
KTEST(Strtol, parses_sign_and_stops_after_the_digits) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtol("   -123456", &end, 10), -123456);
  KEXPECT_EQ(*end, '\0');
  KEXPECT_EQ(klib::strtol("+42 is the answer", &end, 10), 42);
  KEXPECT_EQ(*end, ' ');
}

KTEST(Strtol, takes_any_base_up_to_36) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtol("0x1A", &end, 16), 26);
  KEXPECT_EQ(klib::strtol("-FF", &end, 16), -255);
  KEXPECT_EQ(klib::strtol("101", &end, 2), 5);
  KEXPECT_EQ(klib::strtol("777", &end, 8), 511);
  KEXPECT_EQ(klib::strtol("Zz", &end, 36), 35 * 36 + 35);
  KEXPECT_EQ(klib::strtol("12", &end, 1), 0);
  KEXPECT_EQ(klib::strtol("12", &end, 37), 0);
}

KTEST(Strtol, infers_the_base_from_the_prefix) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtol("0x20", &end, 0), 32);
  KEXPECT_EQ(klib::strtol("0X20", &end, 0), 32);
  KEXPECT_EQ(klib::strtol("010", &end, 0), 8);
  KEXPECT_EQ(klib::strtol("99", &end, 0), 99);
  KEXPECT_EQ(klib::strtol("0", &end, 0), 0);
  KEXPECT_EQ(*end, '\0');
}

// "0x" is a prefix only if a hex digit follows; otherwise the number is the "0" alone.
KTEST(Strtol, does_not_take_a_bare_0x_for_a_prefix) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtol("0xZ", &end, 16), 0);
  KEXPECT_EQ(*end, 'x');
  KEXPECT_EQ(klib::strtol("0xZ", &end, 0), 0);
  KEXPECT_EQ(*end, 'x');
  KEXPECT_EQ(klib::strtol("  -0x", &end, 16), 0);
  KEXPECT_EQ(*end, 'x');
}

KTEST(Strtol, points_back_at_the_start_without_digits) {
  char* end = nullptr;
  const char* empty = "";
  KEXPECT_EQ(klib::strtol(empty, &end, 10), 0);
  KEXPECT_EQ(end, empty);

  const char* words = "   no digits";
  KEXPECT_EQ(klib::strtol(words, &end, 10), 0);
  KEXPECT_EQ(end, words);

  const char* sign_only = "-";
  KEXPECT_EQ(klib::strtol(sign_only, &end, 10), 0);
  KEXPECT_EQ(end, sign_only);
}

KTEST(Strtol, clamps_on_overflow) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtoll("9999999999999999999999999", &end, 10),
             std::numeric_limits<long long>::max());
  KEXPECT_EQ(*end, '\0');
  KEXPECT_EQ(klib::strtoll("-9999999999999999999999999", &end, 10),
             std::numeric_limits<long long>::min());
  KEXPECT_EQ(klib::strtoull("9999999999999999999999999", &end, 10),
             std::numeric_limits<unsigned long long>::max());
  KEXPECT_EQ(klib::strtol("-9223372036854775809", &end, 10), std::numeric_limits<long>::min());
}

// Negating the magnitude of LONG_MIN as a long used to overflow.
KTEST(Strtol, parses_the_most_negative_values_exactly) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtol("-9223372036854775808", &end, 10), std::numeric_limits<long>::min());
  KEXPECT_EQ(*end, '\0');
  KEXPECT_EQ(klib::strtoll("-0x8000000000000000", &end, 0), std::numeric_limits<long long>::min());
  KEXPECT_EQ(*end, '\0');
}

// ISO C: a minus sign negates the unsigned result modulo 2^64.
KTEST(Strtoul, negates_modulo_two_to_the_64) {
  char* end = nullptr;
  KEXPECT_EQ(klib::strtoul("-1", &end, 10), std::numeric_limits<unsigned long>::max());
  KEXPECT_EQ(klib::strtoul("-2", &end, 10), std::numeric_limits<unsigned long>::max() - 1);
  KEXPECT_EQ(klib::strtoull("18446744073709551615", &end, 10),
             std::numeric_limits<unsigned long long>::max());
}
}  // namespace ktest
