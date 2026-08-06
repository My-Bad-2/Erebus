#include <bit>
#include <cstddef>
#include <cstdint>
#include "../include/string.hpp"
#include "stdlib.h"

namespace klib {
	namespace {
		[[gnu::always_inline]] void memswap(void *__restrict a, void *__restrict b,
																				std::size_t size) noexcept {
			using namespace string;

			if (size == 8) [[likely]] {
				const auto temp = unaligned_load<std::uint64_t>(a);
				unaligned_store<std::uint64_t>(a, unaligned_load<std::uint64_t>(b));
				unaligned_store<std::uint64_t>(b, temp);
				return;
			}

			if (size == 4) [[likely]] {
				const auto temp = unaligned_load<std::uint32_t>(a);
				unaligned_store<std::uint32_t>(a, unaligned_load<std::uint32_t>(b));
				unaligned_store<std::uint32_t>(b, temp);
				return;
			}

			char *p1 = static_cast<char *>(a);
			char *p2 = static_cast<char *>(b);

			while (size >= 8) {
				const auto temp = unaligned_load<std::uint64_t>(p1);
				unaligned_store<std::uint64_t>(p1, unaligned_load<std::uint64_t>(p2));
				unaligned_store<std::uint64_t>(p2, temp);
				p1 += 8;
				p2 += 8;
				size -= 8;
			}

			while (size > 0) {
				const char temp = *p1;
				*p1 = *p2;
				*p2 = temp;
				++p1;
				++p2;
				--size;
			}
		}

		using cmp_func_t = int (*)(const void *, const void *);

		void heap_sort(char *base, std::size_t num, std::size_t size,
									 cmp_func_t compar) noexcept {
			if (num < 2) {
				return;
			}

			auto sift_down = [&](std::size_t root, std::size_t end) {
				while (root * 2 + 1 <= end) {
					std::size_t child = root * 2 + 1;

					// Pick the larger child
					if (child + 1 <= end &&
							compar(base + child * size, base + (child + 1) * size) < 0) {
						child++;
					}

					// Swap if child is greater than root
					if (compar(base + root * size, base + child * size) < 0) {
						memswap(base + root * size, base + child * size, size);
						root = child;
					} else {
						break;
					}
				}
			};

			// Heapify
			for (ptrdiff_t i = (num - 2) / 2; i >= 0; --i) {
				sift_down(static_cast<std::size_t>(i), num - 1);
			}

			// Extract max
			for (ptrdiff_t i = num - 1; i > 0; --i) {
				memswap(base, base + i * size, size);
				sift_down(0, static_cast<std::size_t>(i - 1));
			}
		}

		void introsort(char *base, std::size_t num, std::size_t size,
									 cmp_func_t compar, std::size_t depth_limit) noexcept {
			while (num > 16) {
				if (depth_limit == 0) [[unlikely]] {
					heap_sort(base, num, size, compar);
					return;
				}

				depth_limit--;

				std::size_t mid = num / 2;
				if (compar(base + mid * size, base) < 0) {
					memswap(base + mid * size, base, size);
				}

				if (compar(base + (num - 1) * size, base) < 0) {
					memswap(base + (num - 1) * size, base, size);
				}

				if (compar(base + (num - 1) * size, base + mid * size) < 0) {
					memswap(base + (num - 1) * size, base + mid * size, size);
				}

				memswap(base + mid * size, base, size);

				std::size_t left = 1;
				std::size_t right = num - 1;

				while (true) {
					while (left <= right && compar(base + left * size, base) < 0) {
						left++;
					}

					while (left <= right && compar(base + right * size, base) > 0) {
						right--;
					}

					if (left >= right) {
						break;
					}

					memswap(base + left * size, base + right * size, size);
					left++;
					right--;
				}

				memswap(base, base + right * size, size);
				std::size_t pivot_idx = right;

				if (pivot_idx < num / 2) {
					introsort(base, pivot_idx, size, compar, depth_limit);
					base = base + (pivot_idx + 1) * size;
					num = num - (pivot_idx + 1);
				} else {
					introsort(base + (pivot_idx + 1) * size, num - (pivot_idx + 1), size,
										compar, depth_limit);
					num = pivot_idx;
				}
			}
		}
	} // namespace

	void qsort(void *__restrict ptr, std::size_t count, std::size_t size,
						 cmp_func_t compar) noexcept {
		if (count < 2 || size == 0) {
			return;
		}

		char *base = static_cast<char *>(ptr);

		// 2 * log2(count)
		std::size_t depth_limit =
				static_cast<std::size_t>(std::bit_width(count)) * 2;

		introsort(base, depth_limit, size, compar, depth_limit);

		for (std::size_t i = 1; i < count; ++i) {
			for (std::size_t j = i;
					 j > 0 && compar(base + j * size, base + (j - 1) * size) < 0; --j) {
				memswap(base + j * size, base + (j - 1) * size, size);
			}
		}
	}
} // namespace klib
