#include <cstddef>
#include "string.h"

namespace klibc {
	char *strncpy(char *__restrict dest, const char *__restrict src, size_t count) noexcept {
		if (count == 0) [[unlikely]] {
			return dest;
		}

		const std::size_t len = strnlen(src, count);

		memcpy(dest, src, len);

		if (len < count) {
			memset(dest + len, 0, count - len);
		}

		return dest;
	}

	char *strcpy(char *__restrict dest, const char *__restrict src) noexcept {
		const std::size_t len = strlen(src);

		memcpy(dest, src, len + 1);
		return dest;
	}
} // namespace klibc
