/**
 * @file formatters.hpp
 * @brief Core formatting specializations for various data types.
 *
 * This file provides the `klib::formatter<T>` template and its specializations.
 * It is responsible for converting runtime values (strings, integers, pointers,
 * enums, byte spans) into formatted string representations and appending them
 * to a provided buffer sink.
 *
 * @note Formatting Behavior & Specifiers:
 * - Fill & Align: `<` (Left), `>` (Right), `^` (Center). Strings default
 * left, numbers right.
 * - Sign: `+` (Always show), `-` (Show for negatives only), ` ` (Space for
 * positive).
 * - Alt Form (`#`): Adds base prefixes (`0x`, `0b`, `0`).
 * - Zero Pad (`0`): Pads numeric magnitude directly with leading zeros.
 * - Negative Bases: Complies with C++20 `std::format` (e.g., Hex of -26 is
 * `-1a`, not 2's complement).
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace klib {
struct [[gnu::aligned(16)]] FormatSpec {
  int width = 0;
  int precision = -1;
  int base = 10;
  char fill = ' ';
  char align = '\0';
  char sign = '-';
  bool uppercase = false;
  bool alt_form = false;
  std::string_view custom{};
};

namespace detail {
/**
 * @brief Lookup table for base-10 integer-to-string conversion.
 */
inline constexpr std::string_view kDigitPairs =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

/**
 * @brief Helper to write repeated padding characters to a sink.
 *
 * @param sink The target output buffer.
 * @param count Number of characters to write.
 * @param c The character to emit.
 */
template <typename Sink>
constexpr void write_pad(Sink& sink, const int count, char c) noexcept {
  if (count > 0) sink.append_fill(count, c);
}
}  // namespace detail

/**
 * @brief Concept matching standard integers and compiler extensions.
 */
template <typename T>
concept AnyIntegral = std::is_integral_v<T> || requires(T a, T b) {
  { a + b } -> std::same_as<T>;
  { a % b } -> std::same_as<T>;
  { ~a } -> std::same_as<T>;
  requires !std::is_pointer_v<T>;
  requires !std::is_floating_point_v<T>;
};

/**
 * @brief Base Template for type formatting.
 *
 * @tparam T The type to format.
 *
 * @note Types without an explicit specialization or concept match will fall
 * back to this template and output a `[?unsupported?]` tag.
 */
template <typename T>
struct formatter {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T&,
                               const FormatSpec&) noexcept {
    buf.append("[?unsupported?]");
  }
};

template <>
struct formatter<std::string_view> {
  template <typename Sink>
  static constexpr void format(Sink& buf, std::string_view val,
                               const FormatSpec& spec) noexcept {
    if (spec.precision >= 0 &&
        static_cast<std::size_t>(spec.precision) < val.length()) {
      val = val.substr(0, spec.precision);
    }

    const int len = static_cast<int>(val.length());
    const int pad = (spec.width > len) ? spec.width - len : 0;
    const char align = (spec.align != '\0') ? spec.align : '<';

    int p_l = 0, p_r = 0;
    if (align == '>') {
      p_l = pad;
    } else if (align == '<') {
      p_r = pad;
    } else if (align == '^') {
      p_l = pad / 2;
      p_r = pad - p_l;
    }

    detail::write_pad(buf, p_l, spec.fill);
    buf.append(val);
    detail::write_pad(buf, p_r, spec.fill);
  }
};

template <>
struct formatter<const char*> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const char* val,
                               const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(
        buf, val ? std::string_view(val) : "(null)", spec);
  }
};

template <>
struct formatter<char*> : formatter<const char*> {};

template <>
struct formatter<char> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const char val,
                               const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(buf, std::string_view(&val, 1), spec);
  }
};

template <>
struct formatter<bool> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const bool val,
                               const FormatSpec& spec) noexcept {
    formatter<std::string_view>::format(buf, val ? "true" : "false", spec);
  }
};

template <AnyIntegral T>
struct formatter<T> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T val,
                               const FormatSpec& spec) noexcept {
    static constexpr bool is_signed_type = (T(-1) < 0);

    bool is_neg = false;
    char sign_char = '\0';

    if constexpr (is_signed_type) {
      if (val < 0) {
        is_neg = true;
        sign_char = '-';
      }
    }

    if (!is_neg && spec.base == 10) {
      if (spec.sign == '+')
        sign_char = '+';
      else if (spec.sign == ' ')
        sign_char = ' ';
    }

    // 512 bytes supports decimal formatting for _BitInt(1024)
    char temp[512];
    char* ptr = temp + 512;
    const char* hex_digits =
        spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    // Fits in a standard 64-bit hardware register.
    if constexpr (sizeof(T) <= sizeof(std::uint64_t)) {
      std::uint64_t uval = is_neg ? -static_cast<std::uint64_t>(val)
                                  : static_cast<std::uint64_t>(val);

      if (uval == 0) {
        *--ptr = '0';
      } else if (spec.base == 10) {
        while (uval >= 100) {
          const auto m = uval % 100;
          uval /= 100;
          *--ptr = detail::kDigitPairs[(m * 2) + 1];
          *--ptr = detail::kDigitPairs[m * 2];
        }

        if (uval >= 10) {
          *--ptr = detail::kDigitPairs[(uval * 2) + 1];
          *--ptr = detail::kDigitPairs[uval * 2];
        } else {
          *--ptr = static_cast<char>('0' + uval);
        }
      } else {
        do {
          *--ptr = hex_digits[uval % spec.base];
          uval /= spec.base;
        } while (uval > 0);
      }
    } else {  // _BitInt(N) where N > 64.
      if (val == 0) {
        *--ptr = '0';
      } else {
        T copy = val;
        do {
          int rem = static_cast<int>(copy % spec.base);
          if (rem < 0) rem = -rem;

          *--ptr = hex_digits[rem];
          copy /= spec.base;
        } while (copy != 0);
      }
    }

    const int num_len = static_cast<int>((temp + 512) - ptr);
    int precision_pad = spec.precision > num_len ? spec.precision - num_len : 0;

    std::string_view prefix = "";
    if (spec.alt_form && val != 0) {
      if (spec.base == 16)
        prefix = spec.uppercase ? "0X" : "0x";
      else if (spec.base == 2)
        prefix = spec.uppercase ? "0B" : "0b";
      else if (spec.base == 8)
        prefix = "0";
    }

    const int payload_len =
        num_len + precision_pad + (sign_char != '\0' ? 1 : 0) + prefix.length();
    const int width_pad =
        spec.width > payload_len ? spec.width - payload_len : 0;

    char align = (spec.align != '\0') ? spec.align : '>';
    char fill = spec.fill;

    if (fill == '0' && align == '>') {
      precision_pad += width_pad;
      align = '\0';
    }

    int p_l = 0, p_r = 0;
    if (align == '>')
      p_l = width_pad;
    else if (align == '<')
      p_r = width_pad;
    else if (align == '^') {
      p_l = width_pad / 2;
      p_r = width_pad - p_l;
    }

    detail::write_pad(buf, p_l, fill);
    if (sign_char != '\0') buf.push(sign_char);
    buf.append(prefix);
    detail::write_pad(buf, precision_pad, '0');
    buf.append(std::string_view(ptr, num_len));
    detail::write_pad(buf, p_r, fill);
  }
};

template <typename T>
  requires std::is_enum_v<T>
struct formatter<T> {
  template <typename Sink>
  static constexpr void format(Sink& buf, T val,
                               const FormatSpec& spec) noexcept {
    formatter<std::underlying_type_t<T>>::format(
        buf, static_cast<std::underlying_type_t<T>>(val), spec);
  }
};

template <>
struct formatter<const void*> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const void* ptr,
                               FormatSpec spec) noexcept {
    if (!ptr) {
      formatter<std::string_view>::format(buf, "(nil)", spec);
      return;
    }

    spec.base = 16;
    spec.alt_form = true;
    formatter<std::uintptr_t>::format(
        buf, reinterpret_cast<std::uintptr_t>(ptr), spec);
  }
};

template <>
struct formatter<void*> : formatter<const void*> {};

template <>
struct formatter<std::span<const std::byte>> {
  template <typename Sink>
  static constexpr void format(Sink& buf, std::span<const std::byte> dump,
                               const FormatSpec& spec) noexcept {
    const char* hex = spec.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    buf.push('[');
    for (std::size_t i = 0; i < dump.size(); ++i) {
      const auto val = static_cast<std::uint8_t>(dump[i]);
      buf.push(hex[val >> 4]);
      buf.push(hex[val & 0x0F]);
      if (i < dump.size() - 1) buf.push(' ');
    }

    buf.push(']');
  }
};

template <typename T>
struct dynamic_width_wrapper {
  const T& value;
  int width;
};

template <typename T>
constexpr dynamic_width_wrapper<T> dyn(const T& val, int width) noexcept {
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
enum class style : std::uint8_t {
  none = 0,
  bold = 1,
  dim = 2,
  italic = 3,
  underline = 4,
  blink = 5
};
}  // namespace color

template <typename T>
struct ansi_wrapper {
  const T& value;
  color::fg f;
  color::bg b;
  color::style s;
};

template <typename T>
constexpr ansi_wrapper<T> styled(const T& val, color::fg f,
                                 color::bg b = color::bg::none,
                                 color::style s = color::style::none) noexcept {
  return {val, f, b, s};
}

template <typename T>
struct formatter<ansi_wrapper<T>> {
  template <typename Sink>
  static constexpr void format(Sink& sink, const ansi_wrapper<T>& wrap,
                               const FormatSpec& spec) noexcept {
    if (wrap.f == color::fg::none && wrap.b == color::bg::none &&
        wrap.s == color::style::none) {
      formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
      return;
    }

    sink.append("\x1b[");
    bool semi = false;
    FormatSpec num_spec;

    if (wrap.s != color::style::none) {
      formatter<std::uint8_t>::format(sink, static_cast<std::uint8_t>(wrap.s),
                                      num_spec);
      semi = true;
    }

    if (wrap.f != color::fg::none) {
      if (semi) sink.push(';');
      formatter<std::uint8_t>::format(sink, static_cast<std::uint8_t>(wrap.f),
                                      num_spec);
      semi = true;
    }

    if (wrap.b != color::bg::none) {
      if (semi) sink.push(';');
      formatter<std::uint8_t>::format(sink, static_cast<std::uint8_t>(wrap.b),
                                      num_spec);
    }

    sink.push('m');
    formatter<std::decay_t<T>>::format(sink, wrap.value, spec);
    sink.append("\x1b[0m");
  }
};

template <typename T>
concept IterableRange =
    requires(T t) {
      std::begin(t);
      std::end(t);
    } && !std::is_convertible_v<T, std::string_view> &&
    !std::is_same_v<T, std::span<const std::byte>>;

template <IterableRange R>
struct formatter<R> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const R& range,
                               const FormatSpec& spec) noexcept {
    buf.push('[');
    bool first = true;
    for (const auto& item : range) {
      if (!first) buf.append(", ");

      // Format each element recursively
      using ItemType = std::decay_t<decltype(item)>;
      formatter<ItemType>::format(buf, item, spec);
      first = false;
    }

    buf.push(']');
  }
};

template <typename T>
concept HasFormatAs = requires(const T& t) {
  { format_as(t) };
};

template <HasFormatAs T>
struct formatter<T> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const T& val,
                               const FormatSpec& spec) noexcept {
    // Evaluate the hook and forward the result to the existing formatters
    const auto forwarded_val = format_as(val);
    using ForwardedType = std::decay_t<decltype(forwarded_val)>;
    formatter<ForwardedType>::format(buf, forwarded_val, spec);
  }
};
}  // namespace klib
