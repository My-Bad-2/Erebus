/// @file fixed_point.hpp
/// @brief Printing integers that stand for fractions, without floating point.
///
/// `klib::fixed<1000>(uptime_ms)` prints milliseconds as seconds: 12345 becomes `12.345`. The
/// precision of the specifier picks the number of decimals, rounding half up; without one, as
/// many as `Scale` needs.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include "kformat/formatters.hpp"

namespace klib {
/// @brief `value / Scale`, for printing.
template <std::uint64_t Scale, std::integral T>
struct FixedPoint {
  static_assert(Scale > 0, "Scale cannot be zero");
  static_assert(!std::same_as<T, bool> && sizeof(T) <= sizeof(std::uint64_t),
                "FixedPoint holds integers of up to 64 bits");
  T value;

  constexpr explicit FixedPoint(const T v) noexcept : value(v) {}
};

template <std::uint64_t Scale, std::integral T>
[[nodiscard]] constexpr FixedPoint<Scale, T> fixed(const T val) noexcept {
  return FixedPoint<Scale, T>{val};
}

namespace detail {
// Decimals needed to show every step of 1 / scale: as many as scale - 1 has digits.
consteval int default_precision(const std::uint64_t scale) {
  int p = 0;
  for (std::uint64_t s = scale - 1; s > 0; s /= 10) {
    ++p;
  }

  return p == 0 ? 1 : p;
}
}  // namespace detail

template <std::uint64_t Scale, std::integral T>
struct formatter<FixedPoint<Scale, T>> {
  template <typename Sink>
  static constexpr void format(Sink& sink, const FixedPoint<Scale, T>& fx,
                               const FormatSpec& spec) noexcept {
    // Work on the magnitude in the unsigned type, where even the most negative value has one.
    using U = std::make_unsigned_t<T>;
    bool is_neg = false;
    auto magnitude = static_cast<U>(fx.value);
    if constexpr (std::is_signed_v<T>) {
      if (fx.value < 0) {
        is_neg = true;
        magnitude = static_cast<U>(U{0} - magnitude);
      }
    }

    std::uint64_t int_part = static_cast<std::uint64_t>(magnitude) / Scale;
    const std::uint64_t rem = static_cast<std::uint64_t>(magnitude) % Scale;

    const int prec =
        std::min(spec.precision < 0 ? detail::default_precision(Scale) : spec.precision, 18);
    std::uint64_t mult = 1;
    for (int i = 0; i < prec; ++i) {
      mult *= 10;
    }

    // Round half up: (rem * 10^prec + Scale / 2) / Scale, in 128 bits since rem * 10^prec does
    // not fit in 64 for large scales. Rounding up can carry into the integer part.
    auto frac_part = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(rem) * mult + Scale / 2) / Scale);
    if (frac_part >= mult) {
      ++int_part;
      frac_part -= mult;
    }

    // -0.0001 shown with two decimals is just 0.00.
    const bool show_sign = is_neg && (int_part != 0 || frac_part != 0);

    char text[1 + 20 + 1 + 18];
    char* ptr = text + sizeof(text);

    for (int i = 0; i < prec; ++i) {
      *--ptr = static_cast<char>('0' + frac_part % 10);
      frac_part /= 10;
    }

    if (prec > 0) {
      *--ptr = '.';
    }

    do {
      *--ptr = static_cast<char>('0' + int_part % 10);
      int_part /= 10;
    } while (int_part != 0);

    const auto digits_len = static_cast<int>(text + sizeof(text) - ptr);
    const int len = digits_len + (show_sign ? 1 : 0);
    const char align = spec.align != '\0' ? spec.align : '>';

    // As for integers, zero padding goes between the sign and the digits.
    int zeros = 0;
    detail::Padding pad{};
    if (spec.fill == '0' && align == '>') {
      zeros = spec.width > len ? spec.width - len : 0;
    } else {
      pad = detail::split_padding(len, spec.width, align);
    }

    detail::write_pad(sink, pad.left, spec.fill);
    if (show_sign) {
      sink.push('-');
    }

    detail::write_pad(sink, zeros, '0');
    sink.append(std::string_view(ptr, static_cast<std::size_t>(digits_len)));
    detail::write_pad(sink, pad.right, spec.fill);
  }
};
}  // namespace klib
