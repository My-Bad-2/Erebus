#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
alignas(64) std::uint8_t g_a[BUFFER_BYTES];
alignas(64) std::uint8_t g_b[BUFFER_BYTES];

[[nodiscard]] int sign(const int value) noexcept { return (value > 0) - (value < 0); }
}  // namespace

KTEST(Memcmp, finds_the_first_difference_at_every_position) {
  for (const std::size_t count : SIZES) {
    for (std::size_t align_a = 0; align_a < 8; align_a += 3) {
      for (std::size_t align_b = 0; align_b < 8; align_b += 2) {
        std::uint8_t* a = g_a + align_a;
        std::uint8_t* b = g_b + align_b;
        fill_pattern(a, count, 9);
        fill_pattern(b, count, 9);
        KREQUIRE_EQ(klib::memcmp(a, b, count), 0);

        for (std::size_t at = 0; at < count; ++at) {
          // b is greater at `at`; a difference after that, the other way round, must not change
          // the answer.
          a[at] = 0x10;
          b[at] = 0x90;
          if (at + 1 < count) {
            a[count - 1] = 0x90;
            b[count - 1] = 0x10;
          }
          KREQUIRE_EQ(sign(klib::memcmp(a, b, count)), -1);
          KREQUIRE_EQ(sign(klib::memcmp(b, a, count)), 1);

          fill_pattern(a, count, 9);
          fill_pattern(b, count, 9);
        }
      }
    }
  }
}

KTEST(Memcmp, compares_as_unsigned_char) {
  const std::uint8_t low[] = {0x01};
  const std::uint8_t high[] = {0xFF};
  KEXPECT_LT(klib::memcmp(low, high, 1), 0);
  KEXPECT_GT(klib::memcmp(high, low, 1), 0);
  KEXPECT_EQ(klib::memcmp(low, high, 0), 0);
}
}  // namespace ktest::strings
