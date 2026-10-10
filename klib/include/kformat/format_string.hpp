/**
 * @file format_string.hpp
 * @brief Compile-time validated format string wrapper.
 *
 * This file provides the `klib::basic_format_string` template and its
 * associated syntax validator. It performs compile-time verification of curly
 * brace `{}` format specifiers against the provided variadic argument types,
 * ensuring arity and indexing correctness.
 */

#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace klib {
namespace detail {
// These functions are intentionally undefined and non-constexpr. If constant
// evaluation enters a branch calling one of them, the compiler will halt.
[[gnu::error("Cannot mix explicit and implicit format arguments")]]
void compile_error_mixed_format_args();

[[gnu::error("Unescaped '{' inside format placeholder")]]
void compile_error_unescaped_open_brace();

[[gnu::error("Missing closing '}' in format string")]]
void compile_error_missing_close_brace();

[[gnu::error("Unescaped '}' in format string (must be written as '}}')")]]
void compile_error_unescaped_close_brace();

[[gnu::error(
    "Number of implicit format placeholders does not match provided "
    "arguments")]]
void compile_error_implicit_arg_mismatch();

[[gnu::error("Explicit format index out of bounds for provided arguments")]]
void compile_error_explicit_arg_out_of_bounds();

/**
 * @brief Validates format string syntax and argument mapping at compile time.
 *
 * @note This class traverses the string view to assert that the number of
 * format placeholders strictly matches the given variadic argument count,
 * preventing syntactic anomalies like unmatched braces or mixed indexing modes.
 */
class FormatValidator {
  std::string_view m_str;
  std::size_t m_num_args;

  std::size_t m_auto_idx = 0;
  std::size_t m_max_idx = 0;
  bool m_implicit_mode = false;
  bool m_explicit_mode = false;

  constexpr void use_implicit() {
    if (m_explicit_mode) compile_error_mixed_format_args();
    m_implicit_mode = true;

    const std::size_t current_idx = m_auto_idx++;
    if (current_idx + 1 > m_max_idx) m_max_idx = current_idx + 1;
  }

  constexpr void use_explicit(const std::size_t idx) {
    if (m_implicit_mode) compile_error_mixed_format_args();
    m_explicit_mode = true;
    if (idx + 1 > m_max_idx) m_max_idx = idx + 1;
  }

 public:
  /**
   * @brief Constructs a validator for the target format string.
   *
   * @param str The format string to validate.
   * @param num_args The total number of arguments passed for formatting.
   */
  constexpr FormatValidator(std::string_view str, std::size_t num_args) noexcept
      : m_str(str), m_num_args(num_args) {}

  /**
   * @brief Executes the validation pass over the format string.
   */
  constexpr void validate() {
    for (std::size_t i = 0; i < m_str.size(); ++i) {
      if (m_str[i] == '{') {
        // Handle escaped '{{'
        if (i + 1 < m_str.size() && m_str[i + 1] == '{') {
          ++i;
          continue;
        }

        ++i;
        std::size_t arg_idx = 0;
        bool has_explicit = false;

        // Parse positional index (e.g., {1})
        while (i < m_str.size() && m_str[i] >= '0' && m_str[i] <= '9') {
          arg_idx = (arg_idx * 10) + (m_str[i] - '0');
          has_explicit = true;
          ++i;
        }

        if (has_explicit)
          use_explicit(arg_idx);
        else
          use_implicit();

        // Advance to the closing '}', checking for invalid nested '{'
        while (i < m_str.size() && m_str[i] != '}') {
          if (m_str[i] == '{') compile_error_unescaped_open_brace();
          ++i;
        }

        if (i == m_str.size() || m_str[i] != '}')
          compile_error_missing_close_brace();
      } else if (m_str[i] == '}') {
        // Handle escaped '}}'
        if (i + 1 < m_str.size() && m_str[i + 1] == '}')
          ++i;
        else
          compile_error_unescaped_close_brace();
      }
    }

    if (m_implicit_mode && m_auto_idx != m_num_args)
      compile_error_implicit_arg_mismatch();

    if (m_explicit_mode && m_max_idx > m_num_args)
      compile_error_explicit_arg_out_of_bounds();
  }
};
}  // namespace detail

/**
 * @brief A string wrapper evaluated at compile time to ensure valid format
 * parameters.
 *
 * @tparam Args The types of the formatting arguments being evaluated against.
 */
template <typename... Args>
class basic_format_string {
  std::string_view m_format_str;

 public:
  /**
   * @brief Constructs and validates the format string from a string literal.
   *
   * @tparam N The length of the string literal array.
   * @param str A literal representing the format payload.
   */
  template <std::size_t N>
  consteval basic_format_string(const char (&str)[N]) noexcept
      : m_format_str(str, N - 1) {
    detail::FormatValidator validator(m_format_str, sizeof...(Args));
    validator.validate();
  }

  /**
   * @brief Retrieves the validated string view.
   *
   * @return The underlying validated format string.
   */
  [[nodiscard]] constexpr std::string_view get() const noexcept {
    return m_format_str;
  }

  /**
   * @brief Implicit conversion to string view.
   *
   * @return The underlying validated format string.
   */
  constexpr operator std::string_view() const noexcept { return m_format_str; }
};

template <typename... Args>
using format_string = basic_format_string<std::type_identity_t<Args>...>;
}  // namespace klib
