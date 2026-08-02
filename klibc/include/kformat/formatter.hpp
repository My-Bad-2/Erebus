#ifndef EREBUS_KLIBC_INCLUDE_KFORMAT_FORMATTER_HPP
#define EREBUS_KLIBC_INCLUDE_KFORMAT_FORMATTER_HPP

#include <string_view>
#include "utils.hpp"

namespace klibc {
	struct [[gnu::aligned(16)]] FormatSpec {
		int width = 0;
		int base = 10;
		char fill = ' ';
		char align = 0; // '<' -> Left, '>' -> Right, '^' -> Center, 0 -> Default
		bool uppercase = false;
		bool alt_form = false;
	};

	inline constexpr const char *kDigitPairs = "0001020304050607080910111213141516171819"
																						 "2021222324252627282930313233343536373839"
																						 "4041424344454647484950515253545556575859"
																						 "6061626364656667686970717273747576777879"
																						 "8081828384858687888990919293949596979899";

	template<typename T>
	struct formatter {
		template<typename Sink>
		static constexpr void format(Sink &buf, const T &, const FormatSpec &) noexcept {
			buf.append("{?unsupported?}");
		}
	};

	template<>
	struct formatter<std::string_view> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const std::string_view val, const FormatSpec &spec) noexcept {
			const int len = static_cast<int>(val.length());
			const int pad = (spec.width > len) ? spec.width - len : 0;
			const char align = (spec.align != 0) ? spec.align : '<'; // Strings are left-align by default

			int pad_left = 0;
			int pad_right = 0;

			if (align == '>') {
				pad_left = pad;
			} else if (align == '<') {
				pad_right = pad;
			} else if (align == '^') {
				pad_left = pad / 2;
				pad_right = pad - pad_left;
			}

			while (pad_left-- > 0) {
				buf.push(spec.fill);
			}

			buf.append(val);

			while (pad_right-- > 0) {
				buf.push(spec.fill);
			}
		}
	};

	template<>
	struct formatter<const char *> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const char *val, const FormatSpec &spec) noexcept {
			formatter<std::string_view>::format(buf, val ? std::string_view(val) : "(null)", spec);
		}
	};

	template<>
	struct formatter<char *> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const char *val, const FormatSpec &spec) noexcept {
			formatter<const char *>::format(buf, val, spec);
		}
	};

	template<>
	struct formatter<char> {
		template<typename Sink>
		static constexpr void format(Sink &buf, char val, const FormatSpec &spec) noexcept {
			char temp[1] = {val};
			formatter<std::string_view>::format(buf, std::string_view(temp, 1), spec);
		}
	};

	template<>
	struct formatter<bool> {
		template<typename Sink>
		static constexpr void format(Sink &buf, bool val, const FormatSpec &spec) noexcept {
			formatter<std::string_view>::format(buf, val ? "true" : "false", spec);
		}
	};

	template<std::integral T>
	struct formatter<T> {
		template<typename Sink>
		static constexpr void format(Sink &buf, T val, const FormatSpec &spec) noexcept {
			using U = std::make_unsigned_t<T>;
			U uval = val;

			bool is_neg = false;
			if constexpr (std::is_signed_v<T>) {
				if (val < 0 && spec.base == 10) {
					is_neg = true;
					uval = static_cast<U>(-static_cast<U>(val));
				}
			}

			int num_len = 0;
			if (spec.base == 10) {
				num_len = detail::count_digits_10(uval);
			} else if (spec.base == 16) {
				num_len = detail::count_digits_shift(uval, 4);
			} else if (spec.base == 8) {
				num_len = detail::count_digits_shift(uval, 3);
			} else if (spec.base == 2) {
				num_len = detail::count_digits_shift(uval, 1);
			}

			const char *hex_digits = spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

			std::string_view prefix = "";
			if (spec.alt_form && val != 0) {
				if (spec.base == 16) {
					prefix = spec.uppercase ? "0X" : "0x";
				} else if (spec.base == 2) {
					prefix = spec.uppercase ? "0B" : "0b";
				} else if (spec.base == 8) {
					prefix = "0";
				}
			}

			const int total_len = num_len + (is_neg ? 1 : 0) + static_cast<int>(prefix.length());
			const int pad = spec.width > total_len ? spec.width - total_len : 0;
			const char align = (spec.align != 0) ? spec.align : '>'; // Numbers right-align by default

			int pad_left = 0;
			int pad_right = 0;
			int pad_zero = 0;

			if (spec.fill == '0' && align == '>') {
				pad_zero = pad;
			} else {
				if (align == '<') {
					pad_right = pad;
				} else if (align == '>') {
					pad_left = pad;
				} else if (align == '^') {
					pad_left = pad / 2;
					pad_right = pad - pad_left;
				}
			}

			// External Left Pad
			while (pad_left-- > 0) {
				buf.push(spec.fill);
			}

			// Sign and prefix
			if (is_neg) {
				buf.push('-');
			}
			buf.append(prefix);

			// Internal left pad
			while (pad_zero-- > 0) {
				buf.push('0');
			}

			char *direct_out = buf.advance(num_len);

			if (direct_out) {
				std::size_t i = num_len;

				if (spec.base == 10) {
					while (uval >= 100) {
						auto const m = uval % 100;
						uval /= 100;
						direct_out[--i] = kDigitPairs[(m * 2) + 1];
						direct_out[--i] = kDigitPairs[m * 2];
					}

					if (uval >= 10) {
						direct_out[--i] = kDigitPairs[(uval * 2) + 1];
						direct_out[--i] = kDigitPairs[uval * 2];
					} else {
						direct_out[--i] = static_cast<char>('0' + uval);
					}
				} else {
					do {
						direct_out[--i] = hex_digits[uval % spec.base];
						uval /= spec.base;
					} while (uval > 0);
				}
			} else {
				// We hit the boundary of the buffer.
				char temp[64] = {};
				std::size_t i = num_len;

				if (spec.base == 10) {
					while (uval >= 100) {
						auto const m = uval % 100;
						uval /= 100;
						temp[--i] = kDigitPairs[m * 2 + 1];
						temp[--i] = kDigitPairs[m * 2];
					}

					if (uval >= 10) {
						temp[--i] = kDigitPairs[uval * 2 + 1];
						temp[--i] = kDigitPairs[uval * 2];
					} else {
						temp[--i] = static_cast<char>('0' + uval);
					}
				} else {
					do {
						temp[--i] = hex_digits[uval % spec.base];
						uval /= spec.base;
					} while (uval > 0);
				}

				// Push truncated characters forward
				for (std::size_t j = 0; j < static_cast<std::size_t>(num_len); ++j) {
					buf.push(temp[j]);
				}
			}

			// External Right padding
			while (pad_right-- > 0) {
				buf.push(spec.fill);
			}
		}
	};

	template<>
	struct formatter<const void *> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const void *ptr, const FormatSpec &spec) noexcept {
			if (!ptr) {
				formatter<std::string_view>::format(buf, "0x0", spec);
				return;
			}

			buf.append("0x");

			FormatSpec hex_spec;
			hex_spec.base = 16;
			hex_spec.fill = '0';
			hex_spec.align = '>';
			hex_spec.width = sizeof(void *) * 2;

			formatter<std::uintptr_t>::format(buf, reinterpret_cast<std::uintptr_t>(ptr), hex_spec);
		}
	};

	template<>
	struct formatter<void *> : formatter<const void *> {};

	struct hexdump {
		const void *data;
		std::size_t size;
	};

	template<>
	struct formatter<hexdump> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const hexdump &dump, const FormatSpec &spec) noexcept {
			const auto *ptr = static_cast<const unsigned char *>(dump.data);
			const char *hex_digits = spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

			buf.push('[');
			for (std::size_t i = 0; i < dump.size; ++i) {
				buf.push(hex_digits[ptr[i] >> 4]);
				buf.push(hex_digits[ptr[i] & 0x0F]);

				if (i < dump.size - 1) {
					buf.push(' ');
				}
			}

			buf.push(']');
		}
	};

	template<typename T>
	struct dynamic_width_wrapper {
		const T &value;
		int width;
	};

	template<typename T>
	constexpr dynamic_width_wrapper<T> dyn(const T &val, int width) noexcept {
		return {val, width};
	}

	template<typename T>
	struct formatter<dynamic_width_wrapper<T>> {
		template<typename Sink>
		static constexpr void format(Sink &buf, const dynamic_width_wrapper<T> &wrap, FormatSpec spec) noexcept {
			spec.width = wrap.width;
			formatter<std::decay_t<T>>::format(buf, wrap.value, spec);
		}
	};

	namespace fg {
		enum code : std::uint8_t {
			black = 30,
			red = 31,
			green = 32,
			yellow = 33,
			blue = 34,
			magenta = 35,
			cyan = 36,
			white = 37,
			reset = 39,
		};
	} // namespace fg

	namespace bg {
		enum code : std::uint8_t {
			black = 40,
			red = 41,
			green = 42,
			yellow = 43,
			blue = 44,
			magenta = 45,
			cyan = 46,
			white = 47,
			reset = 49,
		};
	} // namespace bg

	namespace text_style {
		enum code : uint8_t {
			none = 0,
			bold = 1,
			dim = 2,
			italic = 3,
			underline = 4,
			blink = 5,
		};
	} // namespace text_style

	template<typename T>
	struct ansi_wrapper {
		const T &value;
		fg::code f;
		bg::code b;
		text_style::code s;
	};

	template<typename T>
	constexpr ansi_wrapper<T> styled(const T &val, fg::code f, bg::code b = bg::reset,
																	 text_style::code s = text_style::none) noexcept {
		return {val, f, b, s};
	}

	template<typename T>
	struct formatter<ansi_wrapper<T>> {
		template<typename Sink>
		static constexpr void format(Sink &sink, const ansi_wrapper<T> &wrap, FormatSpec spec) noexcept {
			if (wrap.f == fg::reset && wrap.b == bg::reset && wrap.s == text_style::none) {
				formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
				return;
			}

			sink.append("\x1b[");
			FormatSpec num_spec;
			bool semi = false;

			if (wrap.s != text_style::none) {
				formatter<uint8_t>::format(sink, wrap.s, num_spec);
				semi = true;
			}

			if (wrap.f != fg::reset) {
				if (semi) {
					sink.push(';');
				}

				formatter<uint8_t>::format(sink, wrap.f, num_spec);
				semi = true;
			}

			if (wrap.b != bg::reset) {
				if (semi) {
					sink.push(';');
				}

				formatter<uint8_t>::format(sink, wrap.b, num_spec);
			}

			sink.push('m'); // Close ANSI Escape
			formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
			sink.append("\x1b[0m");
		}
	};
} // namespace klibc

#endif // EREBUS_KLIBC_INCLUDE_KFORMAT_FORMATTER_HPP
