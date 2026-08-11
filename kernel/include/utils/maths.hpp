#pragma once

#include <bit>
#include <climits>
#include <concepts>
#include <cstdint>

namespace kernel::utils::maths {
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T align_down(T n, U a) noexcept {
  [[assume(std::has_single_bit(a))]];

  return n & ~static_cast<T>(a - 1);
}

template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T align_up(T n, U a) noexcept {
  [[assume(std::has_single_bit(a))]];

  return n + static_cast<T>(a) - 1 & ~static_cast<T>(a - 1);
}

template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T div_round_up(T n, U a) noexcept {
  [[assume(std::has_single_bit(a))]];

  return (n + static_cast<T>(a) - 1) >> std::countr_zero(a);
}

template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T div_round_down(T n,
                                                             U a) noexcept {
  [[assume(std::has_single_bit(a))]];

  return n >> std::countr_zero(a);
}

template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr bool is_aligned(T n, U a) noexcept {
  [[assume(std::has_single_bit(a))]];

  return (n & static_cast<T>(a - 1)) == 0;
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T
Bit_mask(std::size_t bits) noexcept {
  // Shifting by the width of the type is UB
  if (bits >= sizeof(T) * 8) {
    return ~T{0};
  }

  return (T{1} << bits) - 1;
}

template <std::unsigned_integral T, std::integral... U>
[[nodiscard, gnu::always_inline]] constexpr bool
has_bits(T val, U... bit_indices) noexcept {
  return (((val & (T{1} << bit_indices)) != 0) && ...);
}

template <std::unsigned_integral T, std::integral... U>
[[nodiscard, gnu::always_inline]] constexpr bool
has_any_bits(T val, U... bit_indices) noexcept {
  return (((val & (T{1} << bit_indices)) != 0) || ...);
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T log2(T val) noexcept {
  return std::countr_zero(val);
}

template <std::unsigned_integral T = std::size_t>
[[nodiscard, gnu::always_inline]] constexpr T
pow2(std::size_t exponent) noexcept {
  return T{1} << exponent;
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr bool is_pow2(T num) noexcept {
  return std::has_single_bit(num);
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr bool next_pow2(T num) noexcept {
  return std::bit_ceil(num);
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr bool pre_pow2(T num) noexcept {
  return std::bit_floor(num);
}
} // namespace kernel::utils::maths
