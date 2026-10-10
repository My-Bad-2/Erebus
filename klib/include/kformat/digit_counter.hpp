/**
 * @file digit_counter.hpp
 * @brief Digit counting utilities for formatting operations.
 *
 * This file provides optimized functions to calculate the required string
 * length for converting integers into strings. It supports base-10 digit
 * counting using a fast lookup-table approach and base-2 variants (Hex, Octal,
 * Binary) using compile-time boundary mapping.
 */

#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>

namespace klib::detail {
namespace tables {
/**
 * @brief Maps bit widths to their minimum possible base-10 digit count.
 */
inline constexpr std::uint8_t bit_to_dec_guess[65] = {
    1,               // 0 bits
    1,  1,  1,  1,   // 1-4 bits
    2,  2,  2,       // 5-7 bits
    3,  3,  3,       // 8-10 bits
    4,  4,  4,  4,   // 11-14 bits
    5,  5,  5,       // 15-17 bits
    6,  6,  6,       // 18-20 bits
    7,  7,  7,  7,   // 21-24 bits
    8,  8,  8,       // 25-27 bits
    9,  9,  9,       // 28-30 bits
    10, 10, 10, 10,  // 31-34 bits
    11, 11, 11,      // 35-37 bits
    12, 12, 12,      // 38-40 bits
    13, 13, 13, 13,  // 41-44 bits
    14, 14, 14,      // 45-47 bits
    15, 15, 15,      // 48-50 bits
    16, 16, 16, 16,  // 51-54 bits
    17, 17, 17,      // 55-57 bits
    18, 18, 18,      // 58-60 bits
    19, 19, 19, 19   // 61-64 bits
};

/**
 * @brief Powers of 10 used as threshold boundaries for digit count resolution.
 */
inline constexpr std::uint64_t powers_of_10[20] = {
    0ul,
    10ul,
    100ul,
    1000ul,
    10000ul,
    100000ul,
    1000000ul,
    10000000ul,
    100000000ul,
    1000000000ul,
    10000000000ul,
    100000000000ul,
    1000000000000ul,
    10000000000000ul,
    100000000000000ul,
    1000000000000000ul,
    10000000000000000ul,
    100000000000000000ul,
    1000000000000000000ul,
    10000000000000000000ul,
};
}  // namespace tables

/**
 * @brief Calculates the exact number of decimal characters required to
 * represent an unsigned integer.
 *
 * @tparam T An unsigned integral type.
 * @param val The unsigned integer value to measure.
 * @return The number of base-10 digits.
 */
template <std::unsigned_integral T>
[[gnu::always_inline]]
constexpr int count_digits_10(const T val) noexcept {
  if (val == 0) return 1;

  const int bits = std::numeric_limits<T>::digits - std::countl_zero(val);
  const int guess = tables::bit_to_dec_guess[bits];

  // Triggers a boolean addition (0 or 1) depending on threshold crossing
  return guess + (val >= tables::powers_of_10[guess]);
}

/**
 * @brief Calculates the exact number of characters required to represent an
 * unsigned integer in a power-of-2 base.
 *
 * @tparam Shift The base-2 logarithm of the target base (e.g., 1 for binary, 3
 * for octal, 4 for hexadecimal).
 * @tparam T An unsigned integral type.
 * @param val The unsigned integer value to measure.
 * @return The number of characters needed for the representation.
 */
template <int Shift, std::unsigned_integral T>
[[gnu::always_inline]]
constexpr int count_digits_base2(const T val) noexcept {
  static_assert(Shift > 0 && Shift <= 8,
                "Shift must represent a valid power-of-2 base step (1 to 8).");
  if (val == 0) return 1;

  const int bits = std::numeric_limits<T>::digits - std::countl_zero(val);
  return (bits + Shift - 1) / Shift;
}
}  // namespace klib::detail
