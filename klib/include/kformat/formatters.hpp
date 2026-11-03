/// @file formatters.hpp
/// @brief `klib::formatter<T>` and its specializations for strings, integers, pointers, enums,
///        byte spans, ranges and the styling wrappers.
///
/// Specifiers follow std::format where they overlap:
/// - fill and align: `<` left, `>` right, `^` centre. Strings default to left, numbers to right.
/// - sign: `+` always, `-` negatives only (the default), ` ` a space for non-negatives.
/// - `#` adds the base prefix (`0x`, `0b`, `0`); `0` pads with zeros after sign and prefix.
/// - negative numbers keep their sign in every base: `{:x}` of -26 is `-1a`.
///
/// Integers also take a precision, as the minimum number of digits, as printf does.
///
/// A sink needs `push(char)` and `append(std::string_view)`; `append_fill(count, char)` is used
/// for padding when it has one.
#pragma once

#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <type_traits>

namespace klib {
/// @brief One parsed `{:...}` specifier.
struct FormatSpec {
  int width = 0;
  /// -1 when absent.
  int precision = -1;
  int base = 10;
  char fill = ' ';
  /// '\0' when absent, so that each type can pick its own default.
  char align = '\0';
  char sign = '-';
  bool uppercase = false;
  bool alt_form = false;
  /// Everything between ':' and '}', for formatters with a dialect of their own.
  std::string_view custom{};
};

namespace detail {
inline constexpr std::string_view DIGIT_PAIRS =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

/// @brief Writes `count` copies of `c`, nothing if `count` is not positive.
template <typename Sink>
constexpr void write_pad(Sink& sink, const int count, const char c) noexcept {
  if (count <= 0) {
    return;
  }

  if constexpr (requires { sink.append_fill(std::size_t{}, c); }) {
    sink.append_fill(static_cast<std::size_t>(count), c);
  } else {
    for (int i = 0; i < count; ++i) {
      sink.push(c);
    }
  }
}

/// @brief Left and right padding that bring `len` characters up to `spec.width`.
struct Padding {
  int left = 0;
  int right = 0;
};

[[nodiscard]] constexpr Padding split_padding(const int len, const int width,
                                              const char align) noexcept {
  const int pad = width > len ? width - len : 0;
  switch (align) {
    case '<':
      return {0, pad};
    case '^':
      return {pad / 2, pad - pad / 2};
    default:
      return {pad, 0};
  }
}
}  // namespace detail

/// @brief Integer types, including `__int128` and `_BitInt(N)`, which std::is_integral
///        does not always know about.
template <typename T>
concept AnyIntegral = std::is_integral_v<T> || requires(T a, T b) {
  { a + b } -> std::same_as<T>;
  { a % b } -> std::same_as<T>;
  { ~a } -> std::same_as<T>;
  requires !std::is_pointer_v<T>;
  requires !std::is_floating_point_v<T>;
};

/// @brief Formats values of type `T`. Specialize it for your own types.
///
/// Types without a specialization print `[?unsupported?]`.
template <typename T>
struct formatter {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T&, const FormatSpec&) noexcept {
    buf.append("[?unsupported?]");
  }
};

template <>
struct formatter<std::string_view> {
  /// A precision truncates the string.
  template <typename Sink>
  static constexpr void format(Sink& buf, std::string_view val, const FormatSpec& spec) noexcept {
    if (spec.precision >= 0 && static_cast<std::size_t>(spec.precision) < val.length()) {
      val = val.substr(0, static_cast<std::size_t>(spec.precision));
    }

    const auto [left, right] = detail::split_padding(static_cast<int>(val.length()), spec.width,
                                                     spec.align != '\0' ? spec.align : '<');

    detail::write_pad(buf, left, spec.fill);
    buf.append(val);
    detail::write_pad(buf, right, spec.fill);
  }
};

template <>
struct formatter<const char*> {
  /// A null pointer prints `(null)`.
  template <typename Sink>
  static constexpr void format(Sink& buf, const char* val, const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(buf, val ? std::string_view(val) : "(null)", spec);
  }
};

template <>
struct formatter<char*> : formatter<const char*> {};

template <>
struct formatter<char> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const char val, const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(buf, std::string_view(&val, 1), spec);
  }
};

template <>
struct formatter<bool> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const bool val, const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(buf, val ? "true" : "false", spec);
  }
};

template <AnyIntegral T>
struct formatter<T> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T val, const FormatSpec& spec) noexcept {
    constexpr bool is_signed_type = T(-1) < T(0);

    const bool is_neg = is_signed_type && val < T(0);
    char sign_char = '\0';
    if (is_neg) {
      sign_char = '-';
    } else if (spec.sign == '+' || spec.sign == ' ') {
      sign_char = spec.sign;
    }

    // Binary is the longest form: one digit per bit.
    char digits[sizeof(T) * CHAR_BIT];
    char* const digits_end = digits + sizeof(digits);
    char* ptr = digits_end;
    const char* const hex_digits = spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if constexpr (sizeof(T) <= sizeof(std::uint64_t)) {
      // Negating in the unsigned type is defined for the most negative value too.
      std::uint64_t uval = is_neg ? std::uint64_t{0} - static_cast<std::uint64_t>(val)
                                  : static_cast<std::uint64_t>(val);

      if (spec.base == 10) {
        while (uval >= 100) {
          const auto pair = (uval % 100) * 2;
          uval /= 100;
          *--ptr = detail::DIGIT_PAIRS[pair + 1];
          *--ptr = detail::DIGIT_PAIRS[pair];
        }

        if (uval >= 10) {
          *--ptr = detail::DIGIT_PAIRS[uval * 2 + 1];
          *--ptr = detail::DIGIT_PAIRS[uval * 2];
        } else {
          *--ptr = static_cast<char>('0' + uval);
        }
      } else {
        // The other bases are powers of two: shift instead of dividing.
        const int shift = spec.base == 16 ? 4 : spec.base == 8 ? 3 : 1;
        const std::uint64_t mask = (std::uint64_t{1} << shift) - 1;
        do {
          *--ptr = hex_digits[uval & mask];
          uval >>= shift;
        } while (uval != 0);
      }
    } else {
      // Wider than a register: plain division, one digit at a time.
      T rest = val;
      do {
        const auto digit = static_cast<int>(rest % spec.base);
        *--ptr = hex_digits[digit < 0 ? -digit : digit];
        rest /= spec.base;
      } while (rest != 0);
    }

    const int num_len = static_cast<int>(digits_end - ptr);
    int zeros = spec.precision > num_len ? spec.precision - num_len : 0;

    std::string_view prefix;
    if (spec.alt_form && val != T(0)) {
      if (spec.base == 16) {
        prefix = spec.uppercase ? "0X" : "0x";
      } else if (spec.base == 2) {
        prefix = spec.uppercase ? "0B" : "0b";
      } else if (spec.base == 8) {
        prefix = "0";
      }
    }

    const int payload_len =
        (sign_char != '\0' ? 1 : 0) + static_cast<int>(prefix.length()) + zeros + num_len;
    const char align = spec.align != '\0' ? spec.align : '>';

    detail::Padding pad{};
    if (spec.fill == '0' && align == '>') {
      // Zero padding goes between the sign or prefix and the digits.
      zeros += spec.width > payload_len ? spec.width - payload_len : 0;
    } else {
      pad = detail::split_padding(payload_len, spec.width, align);
    }

    detail::write_pad(buf, pad.left, spec.fill);
    if (sign_char != '\0') {
      buf.push(sign_char);
    }

    buf.append(prefix);
    detail::write_pad(buf, zeros, '0');
    buf.append(std::string_view(ptr, static_cast<std::size_t>(num_len)));
    detail::write_pad(buf, pad.right, spec.fill);
  }
};

template <typename T>
  requires std::is_enum_v<T>
struct formatter<T> {
  /// Prints the underlying value.
  template <typename Sink>
  static constexpr void format(Sink& buf, const T val, const FormatSpec& spec) noexcept {
    formatter<std::underlying_type_t<T>>::format(buf, static_cast<std::underlying_type_t<T>>(val),
                                                 spec);
  }
};

template <>
struct formatter<const void*> {
  /// Hexadecimal with a `0x` prefix; a null pointer prints `(nil)`.
  template <typename Sink>
  static constexpr void format(Sink& buf, const void* ptr, FormatSpec spec) noexcept {
    if (ptr == nullptr) {
      formatter<std::string_view>::format(buf, "(nil)", spec);
      return;
    }

    spec.base = 16;
    spec.alt_form = true;
    formatter<std::uintptr_t>::format(buf, reinterpret_cast<std::uintptr_t>(ptr), spec);
  }
};

template <>
struct formatter<void*> : formatter<const void*> {};

namespace detail {
template <typename T>
inline constexpr bool is_byte_span = false;

template <typename B, std::size_t Extent>
  requires std::same_as<std::remove_const_t<B>, std::byte>
inline constexpr bool is_byte_span<std::span<B, Extent>> = true;
}  // namespace detail

template <typename T>
  requires detail::is_byte_span<T>
struct formatter<T> {
  /// A hex dump: `[de ad be ef]`, upper case with `X`. Any span of std::byte, whatever its
  /// extent, so std::as_bytes() of an array works too.
  template <typename Sink>
  static constexpr void format(Sink& buf, const std::span<const std::byte> dump,
                               const FormatSpec& spec) noexcept {
    const char* const hex = spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    buf.push('[');
    for (std::size_t i = 0; i < dump.size(); ++i) {
      if (i != 0) {
        buf.push(' ');
      }

      const auto val = static_cast<std::uint8_t>(dump[i]);
      buf.push(hex[val >> 4]);
      buf.push(hex[val & 0x0F]);
    }

    buf.push(']');
  }
};

/// @brief A value whose field width is only known at run time. See dyn().
template <typename T>
struct dynamic_width_wrapper {
  const T& value;
  int width;
};

/// @brief Formats `val` with field width `width`, overriding any width in the specifier.
template <typename T>
constexpr dynamic_width_wrapper<T> dyn(const T& val, const int width) noexcept {
  return {val, width};
}

template <typename T>
struct formatter<dynamic_width_wrapper<T>> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const dynamic_width_wrapper<T>& wrap,
                               FormatSpec spec) noexcept {
    spec.width = wrap.width;
    formatter<std::decay_t<T>>::format(buf, wrap.value, spec);
  }
};

namespace color {
/// @brief ANSI foreground colours (SGR 30-39).
enum class fg : std::uint8_t {
  none = 0,
  black = 30,
  red = 31,
  green = 32,
  yellow = 33,
  blue = 34,
  magenta = 35,
  cyan = 36,
  white = 37,
  reset = 39
};

/// @brief ANSI background colours (SGR 40-49).
enum class bg : std::uint8_t {
  none = 0,
  black = 40,
  red = 41,
  green = 42,
  yellow = 43,
  blue = 44,
  magenta = 45,
  cyan = 46,
  white = 47,
  reset = 49
};

/// @brief ANSI text styles (SGR 1-5).
enum class style : std::uint8_t {
  none = 0,
  bold = 1,
  dim = 2,
  italic = 3,
  underline = 4,
  blink = 5
};
}  // namespace color

/// @brief A value printed between ANSI escape sequences. See styled().
template <typename T>
struct ansi_wrapper {
  const T& value;
  color::fg f;
  color::bg b;
  color::style s;
};

/// @brief Formats `val` in the given colours and style, then resets all attributes. With
///        everything at `none` no escape sequence is written at all.
template <typename T>
constexpr ansi_wrapper<T> styled(const T& val, const color::fg f,
                                 const color::bg b = color::bg::none,
                                 const color::style s = color::style::none) noexcept {
  return {val, f, b, s};
}

template <typename T>
struct formatter<ansi_wrapper<T>> {
  template <typename Sink>
  static constexpr void format(Sink& sink, const ansi_wrapper<T>& wrap,
                               const FormatSpec& spec) noexcept {
    if (wrap.f == color::fg::none && wrap.b == color::bg::none && wrap.s == color::style::none) {
      formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
      return;
    }

    sink.append("\x1b[");
    bool first = true;
    const auto code = [&sink, &first](const std::uint8_t value) {
      if (value == 0) {
        return;
      }

      if (!first) {
        sink.push(';');
      }

      formatter<std::uint8_t>::format(sink, value, FormatSpec{});
      first = false;
    };

    code(static_cast<std::uint8_t>(wrap.s));
    code(static_cast<std::uint8_t>(wrap.f));
    code(static_cast<std::uint8_t>(wrap.b));
    sink.push('m');

    formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
    sink.append("\x1b[0m");
  }
};

/// @brief Anything iterable that is not a string or a byte span.
template <typename T>
concept IterableRange = requires(T t) {
  std::begin(t);
  std::end(t);
} && !std::is_convertible_v<T, std::string_view> && !detail::is_byte_span<T>;

template <IterableRange R>
struct formatter<R> {
  /// `[a, b, c]`, with the specifier applied to each element.
  template <typename Sink>
  static constexpr void format(Sink& buf, const R& range, const FormatSpec& spec) noexcept {
    buf.push('[');
    bool first = true;
    for (const auto& item : range) {
      if (!first) {
        buf.append(", ");
      }

      formatter<std::decay_t<decltype(item)>>::format(buf, item, spec);
      first = false;
    }

    buf.push(']');
  }
};

/// @brief Types with a `format_as(const T&)` found by argument-dependent lookup, which turns
///        them into something that already has a formatter.
template <typename T>
concept HasFormatAs = requires(const T& t) {
  { format_as(t) };
};

template <HasFormatAs T>
struct formatter<T> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T& val, const FormatSpec& spec) noexcept {
    const auto forwarded = format_as(val);
    formatter<std::decay_t<decltype(forwarded)>>::format(buf, forwarded, spec);
  }
};
}  // namespace klib
