#include "utils/maths.hpp"

#include <cstdint>
#include <limits>

#include "ktest.hpp"

namespace maths = kernel::utils::maths;

KTEST(Maths, alignment) {
  KEXPECT_EQ(maths::align_down(0x1234u, 0x1000u), 0x1000u);
  KEXPECT_EQ(maths::align_up(0x1234u, 0x1000u), 0x2000u);
  KEXPECT_EQ(maths::align_up(0x2000u, 0x1000u), 0x2000u);
  KEXPECT_EQ(maths::align_up(std::uint64_t{0}, 64u), 0u);
  KEXPECT(maths::is_aligned(0x3000ull, 0x1000u));
  KEXPECT(!(maths::is_aligned(0x3001ull, 0x1000u)));
}

KTEST(Maths, rounding_division) {
  KEXPECT_EQ(maths::div_round_up(10u, 3u), 4u);
  KEXPECT_EQ(maths::div_round_up(9u, 3u), 3u);
  KEXPECT_EQ(maths::div_round_up(4097u, 4096u), 2u);
  KEXPECT_EQ(maths::div_round_up(0u, 7u), 0u);
  KEXPECT_EQ(maths::div_round_down(10u, 3u), 3u);
  KEXPECT_EQ(maths::div_round_down(4097u, 4096u), 1u);

  // Must not overflow where (n + a - 1) / a would.
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  KEXPECT_EQ(maths::div_round_up(max, 2u), max / 2 + 1);
  KEXPECT_EQ(maths::div_round_up(max, 3u), max / 3);
}

KTEST(Maths, powers_of_two) {
  KEXPECT_EQ(maths::log2(1u), 0u);
  KEXPECT_EQ(maths::log2(4096u), 12u);
  KEXPECT_EQ(maths::log2(4097u), 12u);
  KEXPECT_EQ(maths::pow2<std::uint64_t>(40), std::uint64_t{1} << 40);
  KEXPECT(maths::is_pow2(64u));
  KEXPECT(!(maths::is_pow2(0u)));
  KEXPECT(!(maths::is_pow2(65u)));
  KEXPECT_EQ(maths::next_pow2(0u), 1u);
  KEXPECT_EQ(maths::next_pow2(5u), 8u);
  KEXPECT_EQ(maths::next_pow2(8u), 8u);
  KEXPECT_EQ(maths::prev_pow2(0u), 0u);
  KEXPECT_EQ(maths::prev_pow2(5u), 4u);
}

KTEST(Maths, bit_helpers) {
  KEXPECT_EQ(maths::bit_mask<std::uint32_t>(0), 0u);
  KEXPECT_EQ(maths::bit_mask<std::uint32_t>(12), 0xFFFu);
  KEXPECT_EQ(maths::bit_mask<std::uint32_t>(32), 0xFFFF'FFFFu);
  KEXPECT_EQ(maths::bit_mask<std::uint32_t>(100), 0xFFFF'FFFFu);
  KEXPECT(maths::has_bits(0b1010u, 1, 3));
  KEXPECT(!(maths::has_bits(0b1010u, 0, 1)));
  KEXPECT(maths::has_any_bits(0b1010u, 0, 1));
  KEXPECT(!(maths::has_any_bits(0b1010u, 0, 2)));
}
