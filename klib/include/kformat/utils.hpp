#pragma once

#include <bit>
#include <cstdint>

namespace klib::detail {
	// The numbers within a specific bit - width range[2 ^ (B - 1), 2 ^ B - 1]
	// vary by less than a factor of 10. Therefore, their decimal length can vary by at most 1.
	inline constexpr std::uint8_t bit_to_dec_guess[65] = {
			1, // 0 bits
			1,	1,	1,	1, // 1-4 bits
			2,	2,	2, // 5-7 bits
			3,	3,	3, // 8-10 bits
			4,	4,	4,	4, // 11-14 bits
			5,	5,	5, // 15-17 bits
			6,	6,	6, // 18-20 bits
			7,	7,	7,	7, // 21-24 bits
			8,	8,	8, // 25-27 bits
			9,	9,	9, // 28-30 bits
			10, 10, 10, 10, // 31-34 bits
			11, 11, 11, // 35-37 bits
			12, 12, 12, // 38-40 bits
			13, 13, 13, 13, // 41-44 bits
			14, 14, 14, // 45-47 bits
			15, 15, 15, // 48-50 bits
			16, 16, 16, 16, // 51-54 bits
			17, 17, 17, // 55-57 bits
			18, 18, 18, // 58-60 bits
			19, 19, 19, 19 // 61-64 bits
	};

	inline constexpr std::uint64_t powers_of_10[20] = {
			0,
			10ULL,
			100ULL,
			1000ULL,
			10000ULL,
			100000ULL,
			1000000ULL,
			10000000ULL,
			100000000ULL,
			1000000000ULL,
			10000000000ULL,
			100000000000ULL,
			1000000000000ULL,
			10000000000000ULL,
			100000000000000ULL,
			1000000000000000ULL,
			10000000000000000ULL,
			100000000000000000ULL,
			1000000000000000000ULL,
			10000000000000000000ULL,
	};

	[[gnu::always_inline]]
	constexpr int count_digits_10(std::uint64_t val) noexcept {
		if (val == 0) {
			return 1;
		}

		int bits = 64 - std::countl_zero(val);

		int guess = detail::bit_to_dec_guess[bits];

		// If the value exceeds the power-of-10 boundary, add 1.
		return guess + (val >= detail::powers_of_10[guess]);
	}

	[[gnu::always_inline]]
	constexpr int count_digits_shift(std::uint64_t val, int shift) noexcept {
		if (val == 0) {
			return 1;
		}

		int bits = 64 - std::countl_zero(val);

		// Exact mathematical mapping for base-2^N boundaries (Hex, Octal, Binary)
		return (bits + shift - 1) / shift;
	}
} // namespace klib::detail
