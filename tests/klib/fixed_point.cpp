#include "fixed_point.hpp"

#include <string.h>

#include <cstdint>
#include <limits>

#include "kformat.hpp"
#include "ktest.hpp"

KTEST(FixedPoint, uptime_formatting) {
  char buf[128] = {0};
  uint64_t uptime_ms = 12345;  // 12 seconds, 345 ms

  klib::kprint(buf, sizeof(buf), "Uptime: {}s", klib::fixed<1000>(uptime_ms));
  KEXPECT_STREQ(buf, "Uptime: 12.345s");

  klib::memset(buf, 0, sizeof(buf));
  klib::kprint(buf, sizeof(buf), "Uptime: {:.2f}s", klib::fixed<1000>(uptime_ms));
  KEXPECT_STREQ(buf, "Uptime: 12.35s");
}

KTEST(FixedPoint, carry_over_rounding_edge_case) {
  char buf[128] = {0};
  uint32_t val = 9999;

  klib::kprint(buf, sizeof(buf), "{:.1f}", klib::fixed<100>(val));
  KEXPECT_STREQ(buf, "100.0");
}

KTEST(FixedPoint, hardware_voltage_math) {
  // Imagine an ADC returns a 12-bit integer where 4096 = 3.3 Volts.
  char buf[128] = {0};
  uint32_t adc_reading = 2048;  // Exactly half (1.65 Volts)

  // adc_reading * 3300 gives millivolts.
  klib::kprint(buf, sizeof(buf), "VCORE: {:.2f}V", klib::fixed<4096>((adc_reading * 3300) / 1000));

  KEXPECT_STREQ(buf, "VCORE: 1.65V");
}

KTEST(FixedPoint, signed_values_and_width_padding) {
  char buf[128] = {0};
  int32_t temp = -456;

  klib::kprint(buf, sizeof(buf), "Temp: {:>5.1f}C", klib::fixed<100>(temp));
  KEXPECT_STREQ(buf, "Temp:  -4.6C");
}

KTEST(FixedPoint, most_negative_value) {
  char buf[64];
  klib::kprint(buf, sizeof(buf), "{}", klib::fixed<1000>(std::numeric_limits<std::int64_t>::min()));
  KEXPECT_STREQ(buf, "-9223372036854775.808");
}

KTEST(FixedPoint, large_scale_does_not_overflow) {
  // rem * 10^prec needs more than 64 bits here.
  char buf[64];
  constexpr std::uint64_t scale = 1'000'000'000'000'000'000ul;
  klib::kprint(buf, sizeof(buf), "{:.18}", klib::fixed<scale>(scale + scale / 4));
  KEXPECT_STREQ(buf, "1.250000000000000000");
}

KTEST(FixedPoint, zero_decimals_rounds) {
  char buf[64];
  klib::kprint(buf, sizeof(buf), "{:.0}|{:.0}", klib::fixed<10>(14u), klib::fixed<10>(15u));
  KEXPECT_STREQ(buf, "1|2");
}

KTEST(FixedPoint, tiny_negative_loses_its_sign) {
  char buf[64];
  klib::kprint(buf, sizeof(buf), "{:.2}", klib::fixed<10000>(-1));
  KEXPECT_STREQ(buf, "0.00");
}

KTEST(FixedPoint, alignment_and_zero_padding) {
  char buf[64];
  klib::kprint(buf, sizeof(buf), "[{:^9.1}]", klib::fixed<10>(-25));
  KEXPECT_STREQ(buf, "[  -2.5   ]");

  klib::kprint(buf, sizeof(buf), "[{:<7.1}]", klib::fixed<10>(25));
  KEXPECT_STREQ(buf, "[2.5    ]");

  klib::kprint(buf, sizeof(buf), "[{:07.1}]", klib::fixed<10>(-25));
  KEXPECT_STREQ(buf, "[-0002.5]");
}
