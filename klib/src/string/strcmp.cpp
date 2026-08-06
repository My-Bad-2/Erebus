#include <limits>


#include "../include/string.hpp"
#include "string.h"

namespace klib {
	int strncmp(const char *s1, const char *s2, size_t count) noexcept {
		using namespace string;
		if (count == 0 || s1 == s2) [[unlikely]] {
			return 0;
		}

		auto p1 = reinterpret_cast<std::uintptr_t>(s1);
		auto p2 = reinterpret_cast<std::uintptr_t>(s2);

		while (count >= 8) {
			if (((p1 & 4095) > 4088) || ((p2 & 4095) > 4088)) [[unlikely]] {
				const auto c1 = *reinterpret_cast<const uint8_t *>(p1);
				const auto c2 = *reinterpret_cast<const uint8_t *>(p2);

				if (c1 != c2 || c1 == '\0') {
					return static_cast<int>(c1) - static_cast<int>(c2);
				}

				++p1;
				++p2;
				--count;
				continue;
			}

			const auto v1 =
					unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(p1));
			const auto v2 =
					unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(p2));

			const std::uint64_t diff = v1 ^ v2;
			const std::uint64_t nulls = has_zero_byte<std::uint64_t>(v1);

			// Exactly 8-byte match and no null terminators found
			if (diff == 0 && nulls == 0) [[likely]] {
				p1 += 8;
				p2 += 8;
				count -= 8;
				continue;
			}

			// Either a mismatch, a null terminator, or both.
			const std::uint32_t diff_shift =
					diff ? (static_cast<std::uint32_t>(std::countr_zero(diff)) & ~7U)
							 : 64;
			const std::uint32_t null_shift =
					nulls ? (static_cast<std::uint32_t>(std::countr_zero(nulls)) & ~7U)
								: 64;

			if (null_shift < diff_shift) {
				return 0;
			}

			const int byte1 = static_cast<std::uint8_t>((v1 >> diff_shift) & 0xFF);
			const int byte2 = static_cast<std::uint8_t>((v2 >> diff_shift) & 0xFF);

			return byte1 - byte2;
		}

		// Tail loop
		while (count > 0) {
			const auto c1 = *reinterpret_cast<const std::uint8_t *>(p1);
			const auto c2 = *reinterpret_cast<const std::uint8_t *>(p2);

			if (c1 != c2 || c1 == '\0') {
				return static_cast<int>(c1) - static_cast<int>(c2);
			}

			++p1;
			++p2;
			--count;
		}

		return 0;
	}

	int strcmp(const char *s1, const char *s2) noexcept {
		return strncmp(s1, s2, std::numeric_limits<std::size_t>::max());
	}
} // namespace klib
