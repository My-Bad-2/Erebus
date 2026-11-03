/// @file kformat.hpp
/// @brief std::format-style formatting into bounded buffers, with format strings checked at
///        compile time.
///
/// @code
/// char buf[64];
/// klib::kprint(buf, sizeof(buf), "{} pages at {:#x}", count, address);
/// @endcode
///
/// format_to() writes into any sink with `push(char)` and `append(std::string_view)`; kprint()
/// wraps a StringBuffer around a char array, truncating and always terminating. Types get their
/// output from klib::formatter, see formatters.hpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "kformat/format_string.hpp"
#include "kformat/formatters.hpp"
#include "kformat/string_buffer.hpp"

namespace klib {
namespace detail {
/// @brief Parses the specifier of one placeholder into `spec`.
///
/// Syntax: `[[fill]align][sign][#][0][width][.precision][type]`, where type is one of
/// `x X b B o p`. Anything else is ignored, but still available to custom formatters as
/// FormatSpec::custom.
///
/// @param pos Index right after the `{` and any argument index.
/// @return Index right after the closing `}`.
constexpr std::size_t parse_spec(const std::string_view view, std::size_t pos,
                                 FormatSpec& spec) noexcept {
  // No specifier: skip to the closing brace.
  if (pos >= view.size() || view[pos] != ':') {
    while (pos < view.size() && view[pos] != '}') {
      ++pos;
    }

    return (pos < view.size() && view[pos] == '}') ? pos + 1 : pos;
  }

  ++pos;  // Skip ':'
  const std::size_t spec_start = pos;

  // Find the end of the specifier block boundary
  std::size_t spec_end = pos;
  while (spec_end < view.size() && view[spec_end] != '}') {
    ++spec_end;
  }

  // Capture the entire raw specifier block in case formatters need it
  spec.custom = view.substr(spec_start, spec_end - spec_start);

  // Parse Fill and Alignment
  if (pos + 1 < spec_end &&
      (view[pos + 1] == '<' || view[pos + 1] == '>' || view[pos + 1] == '^')) {
    spec.fill = view[pos];
    spec.align = view[pos + 1];
    pos += 2;
  } else if (pos < spec_end && (view[pos] == '<' || view[pos] == '>' || view[pos] == '^')) {
    spec.align = view[pos++];
  }

  // Parse Sign Prefix
  if (pos < spec_end && (view[pos] == '+' || view[pos] == '-' || view[pos] == ' ')) {
    spec.sign = view[pos++];
  }

  // Parse Alternate Form
  if (pos < spec_end && view[pos] == '#') {
    spec.alt_form = true;
    ++pos;
  }

  // Parse Implicit Zero-Padding
  if (pos < spec_end && view[pos] == '0' && spec.align == '\0') {
    spec.fill = '0';
    // Zero-padding inherently implies right-alignment for numbers
    spec.align = '>';
    ++pos;
  }

  // Parse Width
  while (pos < spec_end && view[pos] >= '0' && view[pos] <= '9') {
    spec.width = (spec.width * 10) + (view[pos] - '0');
    ++pos;
  }

  // Parse Precision
  if (pos < spec_end && view[pos] == '.') {
    ++pos;
    spec.precision = 0;

    while (pos < spec_end && view[pos] >= '0' && view[pos] <= '9') {
      spec.precision = (spec.precision * 10) + (view[pos] - '0');
      ++pos;
    }
  }

  // Parse Type Code
  if (pos < spec_end) {
    switch (view[pos++]) {
      case 'x':
        spec.base = 16;
        spec.uppercase = false;
        break;
      case 'X':
        spec.base = 16;
        spec.uppercase = true;
        break;
      case 'p':
        spec.base = 16;
        spec.uppercase = false;
        spec.alt_form = true;
        break;
      case 'b':
        spec.base = 2;
        spec.uppercase = false;
        break;
      case 'B':
        spec.base = 2;
        spec.uppercase = true;
        break;
      case 'o':
        spec.base = 8;
        break;
      default:
        break;
    }
  }

  return spec_end < view.size() ? spec_end + 1 : spec_end;
}

/// @brief Formats argument number `target_idx` of the pack. A linear walk, which is fine for
///        the handful of arguments a format string has.
template <std::size_t CurrentIdx = 0, typename Sink, typename T, typename... Ts>
constexpr void dispatch_arg(std::size_t target_idx, Sink& sink, const FormatSpec& spec, T&& arg,
                            Ts&&... args) noexcept {
  if (target_idx == CurrentIdx) {
    formatter<std::decay_t<T>>::format(sink, arg, spec);
    return;
  }

  if constexpr (sizeof...(Ts) > 0)
    dispatch_arg<CurrentIdx + 1>(target_idx, sink, spec, std::forward<Ts>(args)...);
}

// An index past the last argument formats nothing; the format string check rules it out anyway.
template <std::size_t CurrentIdx = 0, typename Sink>
constexpr void dispatch_arg(std::size_t, Sink&, const FormatSpec&) noexcept {}
}  // namespace detail

/// @brief Formats `args` according to `fmt` into `sink`.
///
/// `{{` and `}}` print braces; placeholders are `{}` or `{N}` with an optional `:spec`.
template <typename Sink, typename... Args>
constexpr void format_to(Sink& sink, format_string<Args...> fmt, Args&&... args) noexcept {
  const std::string_view view = fmt.get();
  std::size_t pos = 0;
  std::size_t auto_idx = 0;

  while (pos < view.size()) {
    if (view[pos] == '{') {
      // Escaped '{'
      if (pos + 1 < view.size() && view[pos + 1] == '{') {
        sink.push('{');
        pos += 2;
        continue;
      }

      ++pos;  // Skip '{'
      std::size_t arg_idx = 0;

      // Parse positional argument index
      if (pos < view.size() && view[pos] >= '0' && view[pos] <= '9') {
        while (pos < view.size() && view[pos] >= '0' && view[pos] <= '9') {
          arg_idx = (arg_idx * 10) + (view[pos] - '0');
          ++pos;
        }
      } else {
        arg_idx = auto_idx++;
      }

      FormatSpec spec;
      pos = detail::parse_spec(view, pos, spec);

      detail::dispatch_arg(arg_idx, sink, spec, std::forward<Args>(args)...);
    } else if (view[pos] == '}') {
      // Escaped '}'
      if (pos + 1 < view.size() && view[pos + 1] == '}') {
        sink.push('}');
        pos += 2;
      } else {
        ++pos;  // Malformed unescaped brace.
      }
    } else {
      sink.push(view[pos++]);
    }
  }
}

/// @brief Formats into `buffer`, writing at most `max_size - 1` characters and a terminator.
///
/// @return The length the full output would have had, as snprintf() does: a result of
///         `max_size` or more means it was truncated.
template <typename... Args>
constexpr std::size_t kprint(char* buffer, const std::size_t max_size, format_string<Args...> fmt,
                             Args&&... args) noexcept {
  StringBuffer buf(buffer, max_size);
  format_to(buf, fmt, std::forward<Args>(args)...);
  return buf.finish();
}

/// @brief kprint() into a span.
template <typename... Args>
constexpr std::size_t kprint(const std::span<char> buffer, format_string<Args...> fmt,
                             Args&&... args) noexcept {
  StringBuffer buf(buffer);
  format_to(buf, fmt, std::forward<Args>(args)...);
  return buf.finish();
}
}  // namespace klib
