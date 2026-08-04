#ifndef EREBUS_klib_INCLUDE_KFORMAT_FORMAT_STRING_HPP
#define EREBUS_klib_INCLUDE_KFORMAT_FORMAT_STRING_HPP

namespace klib {
	namespace detail {
		[[gnu::error("Format Argument mismatch")]] void compile_time_error_format_argument_mismatch();
	} // namespace detail

	template<typename... Args>
	class basic_format_string {
		const char *str;

	public:
		template<std::size_t N>
		consteval basic_format_string(const char (&s)[N]) : str(s) {
			std::size_t auto_idx = 0;
			std::size_t max_idx = 0;
			bool implicit_mode = false;
			bool explicit_mode = false;

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
					++p;
					std::size_t arg_idx = 0;

					// Parse positional index (e.g., {1})
					if (*p >= '0' && *p <= '9') {
						explicit_mode = true;

						while (*p >= '0' && *p <= '9') {
							arg_idx = (arg_idx * 10) + (*p - '0');
							++p;
						}
					} else {
						implicit_mode = true;
						arg_idx = auto_idx++;
					}

					if (arg_idx > max_idx) {
						max_idx = arg_idx + 1;
					}

					while (*p && *p != '}') {
						++p;
					}

					if (*p == '}') {
						++p;
					}
				} else {
					++p;
				}
			}

			if (implicit_mode && explicit_mode) {
				detail::compile_time_error_format_argument_mismatch();
			}

			if (implicit_mode && auto_idx != sizeof...(Args)) {
				detail::compile_time_error_format_argument_mismatch();
			}

			if (explicit_mode && max_idx > sizeof...(Args)) {
				detail::compile_time_error_format_argument_mismatch();
			}
		}

		constexpr operator const char *() const noexcept { return str; }
	};

	template<typename... Args>
	using format_string = basic_format_string<std::type_identity_t<Args>...>;
} // namespace klib

#endif // EREBUS_klib_INCLUDE_KFORMAT_FORMAT_STRING_HPP
