#include <bit>
#include <cstddef>
#include <cstdint>
#include "../include/string.hpp"
#include "stdlib.h"

namespace klibc {
	namespace {
		using cmp_func_t = int (*)(const void *, const void *);
	}

	void *bsearch(const void *key, const void *ptr, std::size_t count, std::size_t size, cmp_func_t compar) noexcept {
		if (count == 0 || size == 0) [[unlikely]] {
			return nullptr;
		}

		const char *base = static_cast<const char *>(ptr);

		// Exact match on first element
		if (compar(key, base) == 0) {
			return const_cast<char *>(base);
		}

		// Locates the subset window rapidly.
		std::size_t bound = 1;
		while (bound < count && compar(key, base + bound * size) > 0) {
			bound *= 2;
		}

		std::size_t left = bound / 2;
		std::size_t right = (bound < count) ? bound : count - 1;

		while (left <= right) {
			std::size_t mid = left + (right - left) / 2;
			const char *mid_ptr = base + mid * size;

			int cmp_res = compar(key, mid_ptr);

			if (cmp_res == 0) {
				return const_cast<char *>(mid_ptr);
			}

			if (cmp_res > 0) {
				left = mid + 1;
			} else {
				if (mid == 0) {
					break;
				}

				right = mid - 1;
			}
		}

		return nullptr;
	}
} // namespace klibc
