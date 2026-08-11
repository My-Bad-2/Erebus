#pragma once

#include <algorithm>
#include <cstdint>
#include "kformat/formatter.hpp"

namespace klib {
	template<std::uint64_t Scale, typename T>
	struct FixedPoint {
		static_assert(Scale > 0, "Scale cannot be zero");
		T value;
		constexpr explicit FixedPoint(T value) : value(value) {}
	};

	template<std::uint64_t Scale, typename T>
	[[nodiscard]] constexpr auto fixed(T val) noexcept -> FixedPoint<Scale, T> {
		return FixedPoint<Scale, T>{val};
	}

	namespace detail {
		// Calculates how many decimal digits are needed to fully represent the fractional
		// part if the user doesn't provide a precision limit.
		consteval int default_precision(const std::uint64_t scale) {
			int p = 0;
			std::uint64_t s = scale - 1;

			while (s > 0) {
				p++;
				s /= 10;
			}

			return p == 0 ? 1 : p;
		}
	} // namespace detail

	template<uint64_t Scale, typename T>
	struct formatter<FixedPoint<Scale, T>> {
		template<typename Sink>
		static constexpr void format(Sink &sink, const FixedPoint<Scale, T> &fx, const FormatSpec &spec) noexcept {
			T val = fx.value;
			bool is_neg = false;

			if constexpr (std::is_signed_v<T>) {
				if (val < 0) {
					is_neg = true;
					val = -val;
				}
			}

			auto int_part = static_cast<std::uint64_t>(val) / Scale;
			const auto rem = static_cast<std::uint64_t>(val) % Scale;

			int target_prec = (spec.precision == -1) ? detail::default_precision(Scale) : spec.precision;
			target_prec = std::min(target_prec, 18);

			std::uint64_t mult = 1;
			for (int i = 0; i < target_prec; ++i) {
				mult *= 10;
			}

			std::uint64_t frac_part = 0;
			if (target_prec > 0) {
				// integer rounding: (Remainder * 10^P + Scale/2) / Scale
				frac_part = (rem * mult + (Scale / 2)) / Scale;

				// Carry-over trap
				if (frac_part >= mult) {
					int_part += 1;
					frac_part -= mult;
				}
			} else {
				// If the user requests 0 decimal places, round the integer directly.
				if (rem * 2 >= Scale) {
					int_part += 1;
				}
			}

			const bool show_sign = is_neg && (int_part > 0 || frac_part > 0 || rem > 0);

			int int_len = 1;
			std::uint64_t temp = int_part;
			while (temp >= 10) {
				++int_len;
				temp /= 10;
			}

			int const total_len = (show_sign ? 1 : 0) + int_len + (target_prec > 0 ? 1 + target_prec : 0);
			const int padding = spec.width > total_len ? spec.width - total_len : 0;
			const char align = spec.align == 0 ? '>' : spec.align;

			// Right align padding (spaces)
			if (align == '>' && spec.fill != '0') {
				for (int i = 0; i < padding; ++i) {
					sink.push(spec.fill);
				}
			}

			if (show_sign) {
				sink.push('-');
			}

			// Right align zero-padding
			if (align == '>' && spec.fill == '0') {
				for (int i = 0; i < padding; ++i) {
					sink.push('0');
				}
			}

			// Render raw integers
			char int_buf[20];
			int idx = 0;
			uint64_t temp_val = int_part;
			do {
				int_buf[idx++] = '0' + (temp_val % 10);
				temp_val /= 10;
			} while (temp_val > 0);

			while (idx > 0) {
				sink.push(int_buf[--idx]);
			}

			// Render fraction part
			if (target_prec > 0) {
				sink.push('.');
				char frac_buf[20];
				int f_idx = 0;
				uint64_t temp_f = frac_part;

				// Extract fraction digits
				for (int i = 0; i < target_prec; ++i) {
					frac_buf[f_idx++] = '0' + (temp_f % 10);
					temp_f /= 10;
				}

				while (f_idx > 0) {
					sink.push(frac_buf[--f_idx]);
				}
			}

			// Left algin padding
			if (align == '<') {
				for (int i = 0; i < padding; ++i) {
					sink.push(spec.fill);
				}
			}
		}
	};
} // namespace klib
