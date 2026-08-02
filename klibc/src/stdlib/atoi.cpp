#include <cstdint>
#include <type_traits>
#include "ctype.h"
#include "stdlib.h"

namespace klibc {
	namespace {
		template<typename T>
			requires std::is_integral_v<T> && std::is_signed_v<T>
		[[gnu::always_inline]] constexpr T atoX(const char *str) noexcept {
			while (isspace(*str)) {
				++str;
			}

			bool neg = false;
			if (*str == '-') {
				neg = true;
				++str;
			} else if (*str == '+') {
				++str;
			}

			using U = std::make_unsigned_t<T>;
			U res = 0;

			std::uint8_t c = static_cast<uint8_t>(*str - '0');
			while (c < 10) {
				res = res * 10 + c;
				c = static_cast<uint8_t>(*(++str) - '0');
			}

			return static_cast<T>(neg ? -res : res);
		}
	} // namespace

	int atoi(const char *str) noexcept { return atoX<int>(str); }

	long atol(const char *str) noexcept { return atoX<long>(str); }

	long long atoll(const char *str) noexcept { return atoX<long long>(str); }
} // namespace klibc
