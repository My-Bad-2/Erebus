#ifndef EREBUS_KLIBC_INCLUDE_KFORMAT_HPP
#define EREBUS_KLIBC_INCLUDE_KFORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kformat/format_buffer.hpp"
#include "kformat/format_string.hpp"
#include "kformat/formatter.hpp"

namespace klibc {
	namespace detail {
		constexpr const char *parse_spec(const char *p, FormatSpec &spec) noexcept {
			// If there's no format specifier, fast forward and exit.
			if (*p != ':') {
				while (*p != '\0' && *p != '}') {
					p++;
				}

				if (*p == '}') {
					p++;
				}

				return p;
			}

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

			// Skip any trailing garbage until we hit the closing brace
			while (*p != '\0' && *p != '}') {
				p++;
			}

			if (*p == '}') {
				p++;
			}

			return p;
		}
	} // namespace detail

	template<typename Sink, typename... Args>
	constexpr void format_to(Sink &sink, format_string<Args...> fmt, Args &&...args) noexcept {
		const char *p = fmt;

		auto process_one_arg = [&]<typename T0>(T0 &&arg) {
			while (*p) {
				if (*p == '{' && *(p + 1) == '{') {
					sink.push('{');
					p += 2;
				} else if (*p == '}' && *(p + 1) == '}') {
					sink.push('}');
					p += 2;
				} else if (*p == '{') {
					FormatSpec spec;
					p = detail::parse_spec(p + 1, spec);

					formatter<std::decay_t<T0>>::format(sink, arg, spec);
					return;
				} else {
					sink.push(*p++);
				}
			}
		};

		(process_one_arg(std::forward<Args>(args)), ...);

		while (*p) {
			if (*p == '{' && *(p + 1) == '{') {
				sink.push('{');
				p += 2;
			} else if (*p == '}' && *(p + 1) == '}') {
				sink.push('}');
				p += 2;
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
} // namespace klibc

#endif // EREBUS_KLIBC_INCLUDE_KFORMAT_HPP
