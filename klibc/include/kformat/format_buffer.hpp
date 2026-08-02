#ifndef EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_BUFFER_HPP
#define EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_BUFFER_HPP

#include <cstddef>
#include <string_view>

namespace klibc {
	class StringBuffer {
		char *current;
		char *const end;
		std::size_t capacity = 0;
		std::size_t total_written = 0;

	public:
		constexpr StringBuffer(char *buf, const std::size_t max_size) noexcept :
				current(buf), end(buf + (max_size > 0 ? max_size - 1 : 0)), capacity(max_size) {}

		[[gnu::always_inline]]
		constexpr void push(const char c) noexcept {
			if (current < end) {
				*current++ = c;
			}

			total_written++;
		}

		constexpr void append(std::string_view sv) noexcept {
			const std::size_t len = sv.length();

			if (current < end) {
				const std::size_t avail = static_cast<std::size_t>(end - current);
				const std::size_t copy_len = (len < avail) ? len : avail;

				if (std::is_constant_evaluated()) {
					for (std::size_t i = 0; i < copy_len; ++i) {
						current[i] = sv[i];
					}
				} else {
					__builtin_memcpy(current, sv.data(), copy_len);
				}

				current += copy_len;
			}

			total_written += len;
		}

		constexpr std::size_t finish() const noexcept {
			if (capacity > 0 && current) {
				*current = '\0';
			}

			return total_written;
		}

		constexpr char *advance(const std::size_t n) noexcept {
			const std::size_t avail = (current < end) ? static_cast<std::size_t>(end - current) : 0;

			if (n <= avail) {
				char *ptr = current;
				current += n;
				total_written += n;
				return ptr;
			}

			return nullptr;
		}
	};
} // namespace klibc

#endif // EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_BUFFER_HPP
