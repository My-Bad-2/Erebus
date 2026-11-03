/// @file maths.hpp
/// @brief Integer helpers for alignment, rounding division and powers of two.

#pragma once

#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>

namespace kernel::utils::maths {
/// @brief Rounds @p n down to a multiple of @p a, which must be a power of two.
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T align_down(T n, U a) noexcept {
  [[assume(a != 0 && (a & (a - 1)) == 0)]];
  return n & ~static_cast<T>(a - 1);
}

/// @brief Rounds @p n up to a multiple of @p a, which must be a power of two.
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T align_up(T n, U a) noexcept {
  [[assume(a != 0 && (a & (a - 1)) == 0)]];
  return (n + static_cast<T>(a - 1)) & ~static_cast<T>(a - 1);
}

/// @brief `ceil(n / a)` for any non-zero @p a, without overflowing for large @p n.
///
/// Powers of two take a shift instead of a division. When @p a is a constant the compiler
/// picks the right form by itself; the branch only matters for run-time divisors.
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T div_round_up(T n, U a) noexcept {
  [[assume(a > 0)]];
  const auto d = static_cast<T>(a);
  if (std::has_single_bit(d)) {
    return (n >> std::countr_zero(d)) + ((n & (d - 1)) != 0);
  }

  return (n / d) + (n % d != 0);
}

/// @brief `floor(n / a)` for any non-zero @p a.
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr T div_round_down(T n, U a) noexcept {
  [[assume(a > 0)]];
  const auto d = static_cast<T>(a);
  return std::has_single_bit(d) ? n >> std::countr_zero(d) : n / d;
}

/// @brief Whether @p n is a multiple of @p a, which must be a power of two.
template <std::unsigned_integral T, std::unsigned_integral U>
[[nodiscard, gnu::always_inline]] constexpr bool is_aligned(T n, U a) noexcept {
  [[assume(a != 0 && (a & (a - 1)) == 0)]];
  return (n & static_cast<T>(a - 1)) == 0;
}

/// @brief A mask of the low @p bits bits. Saturates at the width of @p T.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T bit_mask(const std::size_t bits) noexcept {
  // Shifting by the width of the type is UB.
  if (bits >= sizeof(T) * CHAR_BIT) {
    return ~T{0};
  }

  return (T{1} << bits) - 1;
}

/// @brief Whether every one of the given bit indices is set in @p val.
template <std::unsigned_integral T, std::integral... U>
[[nodiscard, gnu::always_inline]] constexpr bool has_bits(T val, U... bit_indices) noexcept {
  const T mask = (T{0} | ... | (T{1} << bit_indices));
  return (val & mask) == mask;
}

/// @brief Whether any of the given bit indices is set in @p val.
template <std::unsigned_integral T, std::integral... U>
[[nodiscard, gnu::always_inline]] constexpr bool has_any_bits(T val, U... bit_indices) noexcept {
  return (val & (T{0} | ... | (T{1} << bit_indices))) != 0;
}

/// @brief `floor(log2(val))`. @p val must not be zero.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T log2(T val) noexcept {
  [[assume(val != 0)]];
  return static_cast<T>(std::bit_width(val) - 1);
}

template <std::unsigned_integral T = std::size_t>
[[nodiscard, gnu::always_inline]] constexpr T pow2(std::size_t exponent) noexcept {
  return T{1} << exponent;
}

template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr bool is_pow2(T num) noexcept {
  return std::has_single_bit(num);
}

/// @brief Smallest power of two that is at least @p num (1 for 0).
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T next_pow2(T num) noexcept {
  return std::bit_ceil(num);
}

/// @brief Largest power of two that is at most @p num (0 for 0).
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] constexpr T prev_pow2(T num) noexcept {
  return std::bit_floor(num);
}
}  // namespace kernel::utils::maths
