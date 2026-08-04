#ifndef EREBUS_klib_INCLUDE_KFORMAT_HPP
#define EREBUS_klib_INCLUDE_KFORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kformat/format_buffer.hpp"
#include "kformat/format_string.hpp"
#include "kformat/formatter.hpp"

namespace klib {
	namespace detail {
		constexpr const char *parse_spec(const char *p, FormatSpec &spec) noexcept {
			// If there's no format specifier, fast forward and exit.
			if (*p != ':') {
				while (*p != '\0' && *p != '}') {
					++p;
				}

				if (*p == '}') {
					++p;
				}

				return p;
			}

			const char *spec_start = p + 1;
			const char *spec_end = spec_start;
			while (*spec_end != '\0' && *spec_end != '}') {
				spec_end++;
			}

			spec.custom = std::string_view(spec_start, static_cast<std::size_t>(spec_end - spec_start));

			p++; // skip ':'

			// Lookahead to see if the next character is an alignment char
			if (*p != '\0' && *(p + 1) != '\0' && (*(p + 1) == '<' || *(p + 1) == '>' || *(p + 1) == '^')) {
				spec.fill = *p;
				spec.align = *(p + 1);
				p += 2;
			} else if (*p == '<' || *p == '>' || *p == '^') {
				spec.align = *p++;
			}

			// Parse Alternate Form [#]
			if (*p == '#') {
				spec.alt_form = true;
				p++;
			}

			//	Parse Implicit Zero-Padding [0]
			if (*p == '0' && spec.align == 0) {
				spec.fill = '0';
				spec.align = '>'; // Zero-padding implies right-alignment
				p++;
			}

			// Parse width
			while (*p >= '0' && *p <= '9') {
				spec.width = (spec.width * 10) + (*p - '0');
				p++;
			}

			// Parse precision
			if (*p == '.') {
				p++;
				spec.precision = 0;

				while (*p >= '0' && *p <= '9') {
					spec.precision = (spec.precision * 10) + (*p - '0');
					p++;
				}
			}

			// Parse type
			switch (*p) {
				case 'x': {
					spec.base = 16;
					spec.uppercase = false;
					p++;
					break;
				}
				case 'X': {
					spec.base = 16;
					spec.uppercase = true;
					p++;
					break;
				}
				case 'p': {
					spec.base = 16;
					spec.uppercase = false;
					p++;
					break;
				}
				case 'b': {
					spec.base = 2;
					p++;
					break;
				}
				case 'o': {
					spec.base = 8;
					p++;
					break;
				}
				default:
					break;
			}

			// forward past the specifier block
			p = spec_end;
			if (*p == '}') {
				p++;
			}

			return p;
		}
	} // namespace detail

	template<typename Sink, typename... Args>
	constexpr void format_to(Sink &sink, format_string<Args...> fmt, Args &&...args) noexcept {
		const char *p = fmt;
		std::size_t auto_idx = 0;

		auto print_arg = [&](std::size_t target_idx, const FormatSpec &spec) {
			std::size_t curr = 0;
			bool found = false;

			auto check = [&]<typename T0>(T0 &&arg) {
				if (curr == target_idx && !found) {
					using Type = std::decay_t<T0>;
					formatter<Type>::format(sink, arg, spec);
					found = true;
				}

				++curr;
			};

			(check(std::forward<Args>(args)), ...);
		};

		while (*p) {
			if (*p == '{' && *(p + 1) == '{') {
				sink.push('{');
				p += 2;
			} else if (*p == '}' && *(p + 1) == '}') {
				sink.push('}');
				p += 2;
			} else if (*p == '{') {
				++p;
				std::size_t arg_idx = 0;

				if (*p >= '0' && *p <= '9') {
					while (*p >= '0' && *p <= '9') {
						arg_idx = (arg_idx * 10) + (*p - '0');
						++p;
					}
				} else {
					arg_idx = auto_idx++;
				}

				FormatSpec spec;
				p = detail::parse_spec(p, spec);

				print_arg(arg_idx, spec);
			} else {
				sink.push(*p++);
			}
		}
	}

	template<typename... Args>
	constexpr std::size_t kprint(char *buffer, const std::size_t max_size, format_string<Args...> fmt,
															 Args &&...args) noexcept {
		StringBuffer buf(buffer, max_size);
		format_to(buf, fmt, std::forward<Args>(args)...);
		return buf.finish();
	}
} // namespace klib

#endif // EREBUS_klib_INCLUDE_KFORMAT_HPP
