/// @file string_buffer.hpp
/// @brief A bounded, non-owning character buffer that formatting can write into.
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace klib {
/// @brief Counts what was written into a buffer of fixed capacity, one byte of which is kept for
///        the terminator.
///
/// Writes past the end are counted but not stored, so the total says how large the buffer would
/// have had to be.
class WriteTracker {
  std::size_t m_capacity;
  std::size_t m_total_written{0};

 public:
  constexpr explicit WriteTracker(const std::size_t max_capacity) noexcept
      : m_capacity(max_capacity) {}

  /// @brief How many characters fit, leaving room for the terminator.
  [[nodiscard]] constexpr std::size_t writable_capacity() const noexcept {
    return m_capacity > 0 ? m_capacity - 1 : 0;
  }

  /// @brief Where the next stored character goes.
  [[nodiscard]] constexpr std::size_t current_offset() const noexcept {
    return std::min(m_total_written, writable_capacity());
  }

  /// @brief How many more characters fit.
  [[nodiscard]] constexpr std::size_t available_space() const noexcept {
    return writable_capacity() - current_offset();
  }

  constexpr void record_write(const std::size_t n) noexcept { m_total_written += n; }

  /// @brief Every character written so far, stored or not.
  [[nodiscard]] constexpr std::size_t total_written() const noexcept { return m_total_written; }
};

/// @brief A format sink over a caller's char array. Never writes past the end, and finish()
///        always terminates the text when the array has room for anything at all.
class StringBuffer {
  std::span<char> m_buffer;
  WriteTracker m_tracker;

 public:
  constexpr explicit StringBuffer(const std::span<char> buffer) noexcept
      : m_buffer(buffer), m_tracker(buffer.size()) {}

  constexpr StringBuffer(char* buf, const std::size_t max_size) noexcept
      : m_buffer(buf, max_size), m_tracker(max_size) {}

  /// @brief Appends `c`, or just counts it once the buffer is full.
  [[gnu::always_inline]] constexpr void push(const char c) noexcept {
    if (m_tracker.available_space() > 0) {
      m_buffer[m_tracker.current_offset()] = c;
    }

    m_tracker.record_write(1);
  }

  /// @brief Appends as much of `sv` as fits; counts all of it.
  constexpr void append(const std::string_view sv) noexcept {
    const std::size_t copy_len = std::min(sv.length(), m_tracker.available_space());
    if (copy_len > 0) {
      std::ranges::copy_n(sv.begin(), copy_len, m_buffer.begin() + m_tracker.current_offset());
    }

    m_tracker.record_write(sv.length());
  }

  /// @brief Appends `count` copies of `c`, as many as fit; counts all of them.
  constexpr void append_fill(const std::size_t count, const char c) noexcept {
    const std::size_t fill_len = std::min(count, m_tracker.available_space());
    if (fill_len > 0) {
      std::ranges::fill_n(m_buffer.begin() + m_tracker.current_offset(), fill_len, c);
    }

    m_tracker.record_write(count);
  }

  /// @brief Terminates the text.
  /// @return The length the text would have had without truncation.
  constexpr std::size_t finish() noexcept {
    if (!m_buffer.empty()) {
      m_buffer[m_tracker.current_offset()] = '\0';
    }

    return m_tracker.total_written();
  }

  /// @brief Reserves `n` characters for the caller to fill in, or returns an empty span if they
  ///        do not all fit. Nothing is counted in that case.
  constexpr std::span<char> advance(const std::size_t n) noexcept {
    if (n > m_tracker.available_space()) {
      return {};
    }

    const std::span<char> block = m_buffer.subspan(m_tracker.current_offset(), n);
    m_tracker.record_write(n);
    return block;
  }

  /// @brief The characters stored so far.
  [[nodiscard]] constexpr std::span<char> active_span() noexcept {
    return m_buffer.first(m_tracker.current_offset());
  }

  /// @copydoc active_span()
  [[nodiscard]] constexpr std::span<const char> active_span() const noexcept {
    return m_buffer.first(m_tracker.current_offset());
  }

  /// @brief The characters stored so far, as a string view.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {m_buffer.data(), m_tracker.current_offset()};
  }
};
}  // namespace klib
