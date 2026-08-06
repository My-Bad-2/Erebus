#include <cstdint>
#include <type_traits>
#include "stdlib.h"

namespace klib {
	namespace {
		template<typename T>
			requires std::is_integral_v<T> && std::is_signed_v<T>
		[[gnu::const, gnu::always_inline]]
		constexpr T abs_impl(T x) noexcept {
			using U = std::make_unsigned_t<T>;

			// If x >= 0, mask = 0
			// If x < 0, mask = (unsigned)(-1)
			const U mask = static_cast<U>(x >> (sizeof(T) * 8 - 1));

			// Positive: (x ^ 0) - 0 = x
			// Negative = (x ^ -1) - (-1) = ~x + 1 = -x
			return static_cast<T>((static_cast<U>(x) ^ mask) - mask);
		}

		template<typename DivStruct, typename T>
			requires std::is_integral_v<T> && std::is_signed_v<T>
		[[gnu::const, gnu::always_inline]]
		constexpr DivStruct div_impl(T numer, T denom) noexcept {
			return DivStruct{
					.quot = numer / denom,
					.rem = numer % denom,
			};
		}
	} // namespace

	int abs(int j) noexcept { return abs_impl<int>(j); }

	long labs(long j) noexcept { return abs_impl<long>(j); }

	long long llabs(long long j) noexcept { return abs_impl<long long>(j); }

	std::intmax_t imaxabs(std::intmax_t j) noexcept {
		return abs_impl<std::intmax_t>(j);
	}

	div_t div(int numer, int denom) noexcept {
		return div_impl<div_t, int>(numer, denom);
	}

	ldiv_t ldiv(long numer, long denom) noexcept {
		return div_impl<ldiv_t, long>(numer, denom);
	}

	lldiv_t lldiv(long long numer, long long denom) noexcept {
		return div_impl<lldiv_t, long long>(numer, denom);
	}

	imaxdiv_t imaxdiv(std::intmax_t numer, std::intmax_t denom) noexcept {
		return div_impl<imaxdiv_t, std::intmax_t>(numer, denom);
	}
} // namespace klib
