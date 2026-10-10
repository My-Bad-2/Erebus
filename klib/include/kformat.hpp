/**
 * @file kformat.hpp
 * @brief Main frontend and parser for the string formatting engine.
 *
 * This file coordinates the formatting pipeline by parsing runtime format
 * specifiers, matching them with the compile-time verified `format_string`
 * indices, and safely dispatching the corresponding arguments to their
 * respective type formatters.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kformat/format_string.hpp"
#include "kformat/formatters.hpp"
#include "kformat/string_buffer.hpp"

namespace klib {
namespace detail {
/**
 * @brief Parses formatting directives inside a curly-brace placeholder.
 *
 * @param view The full format string view.
 * @param pos The current index positioned immediately after the parsed argument
 * index or `{`.
 * @param spec The `FormatSpec` instance to populate with parsed directives.
 * @return The advanced string index positioned exactly after the closing `}`.
 *
 * @note Syntax rules: `[[fill]align][sign][#][0][width][.precision][type]`
 */
constexpr std::size_t parse_spec(const std::string_view view, std::size_t pos,
                                 FormatSpec& spec) noexcept {
  // If there's no format specifier (e.g. `{0}` or `{}`), fast forward to
  // closing brace.
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
  } else if (pos < spec_end &&
             (view[pos] == '<' || view[pos] == '>' || view[pos] == '^')) {
    spec.align = view[pos++];
  }

  // Parse Sign Prefix
  if (pos < spec_end &&
      (view[pos] == '+' || view[pos] == '-' || view[pos] == ' ')) {
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

/**
 * @brief Dispatcher resolving a runtime index to a variadic template argument.
 *
 * @tparam CurrentIdx The recursion counter.
 * @param target_idx The resolved runtime format positional index.
 * @param sink The buffer output sink.
 * @param spec The format specifications.
 * @param arg The active head argument.
 * @param args The remaining argument tail.
 */
template <std::size_t CurrentIdx = 0, typename Sink, typename T, typename... Ts>
constexpr void dispatch_arg(std::size_t target_idx, Sink& sink,
                            const FormatSpec& spec, T&& arg,
                            Ts&&... args) noexcept {
  if (target_idx == CurrentIdx) {
    formatter<std::decay_t<T>>::format(sink, arg, spec);
    return;
  }

  if constexpr (sizeof...(Ts) > 0)
    dispatch_arg<CurrentIdx + 1>(target_idx, sink, spec,
                                 std::forward<Ts>(args)...);
}

// No-op dispatcher when parameter pack is empty
template <std::size_t CurrentIdx = 0, typename Sink>
constexpr void dispatch_arg(std::size_t, Sink&, const FormatSpec&) noexcept {}
}  // namespace detail

/**
 * @brief Processes a verified format string and dispatches outputs to a sink
 * buffer.
 *
 * @param sink The target sink object exposing `.push(char)` and
 * `.append(string_view)`.
 * @param fmt The compile-time validated format string struct.
 * @param args Variadic payload containing formatting values.
 */
template <typename Sink, typename... Args>
constexpr void format_to(Sink& sink, format_string<Args...> fmt,
                         Args&&... args) noexcept {
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

/**
 * @brief Pointer-and-size wrapper for formatted printing into a memory block.
 *
 * @param buffer Raw char pointer to target block.
 * @param max_size Absolute bound of the target memory array (including
 * terminator).
 * @param fmt Verified formatting payload.
 * @param args Target variables.
 * @return Total number of bytes written (or attempted, if truncated).
 */
template <typename... Args>
constexpr std::size_t kprint(char* buffer, const std::size_t max_size,
                             format_string<Args...> fmt,
                             Args&&... args) noexcept {
  StringBuffer buf(buffer, max_size);
  format_to(buf, fmt, std::forward<Args>(args)...);
  return buf.finish();
}

/**
 * @brief Span-based wrapper for formatted printing into a memory block.
 *
 * @param buffer Bounds-checked span describing the memory payload.
 * @param fmt Verified formatting payload.
 * @param args Target variables.
 * @return Total number of bytes written (or attempted, if truncated).
 */
template <typename... Args>
constexpr std::size_t kprint(const std::span<char> buffer,
                             format_string<Args...> fmt,
                             Args&&... args) noexcept {
  StringBuffer buf(buffer);
  format_to(buf, fmt, std::forward<Args>(args)...);
  return buf.finish();
}
}  // namespace klib
