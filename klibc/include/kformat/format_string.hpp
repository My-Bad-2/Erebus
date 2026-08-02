#ifndef EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_STRING_HPP
#define EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_STRING_HPP

namespace klibc {
	namespace detail {
		[[gnu::error("Format Argument mismatch")]] void compile_time_error_format_argument_mismatch();
	} // namespace detail

	template<typename... Args>
	class basic_format_string {
		const char *str;

	public:
		template<std::size_t N>
		consteval basic_format_string(const char (&s)[N]) : str(s) {
			std::size_t placeholder_count = 0;
			const char *p = s;

			while (*p) {
				if (*p == '{' && *(p + 1) == '{') {
					p += 2;
					continue;
				}

				if (*p == '}' && *(p + 1) == '}') {
					p += 2;
					continue;
				}

				if (*p == '{') {
					placeholder_count++;
					p++;

					while (*p && *p != '}') {
						p++;
					}

					if (*p == '}') {
						p++;
					}
				} else {
					p++;
				}
			}

			if (placeholder_count != sizeof...(Args)) {
				detail::compile_time_error_format_argument_mismatch();
			}
		}

		constexpr operator const char *() const noexcept { return str; }
	};

	template<typename... Args>
	using format_string = basic_format_string<std::type_identity_t<Args>...>;
} // namespace klibc

#endif // EREBUS_KLIBC_INCLUDE_KFORMAT_FORMAT_STRING_HPP
