/**
 * @file string_buffer.hpp
 * @brief Bounded, non-owning string buffer and write tracking utilities.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace klib {
/**
 * @brief Tracks writing progress and remaining capacity for bounded buffer
 * operations.
 *
 * @note Maintains the state necessary to calculate string truncation limits
 * and formatting lengths while reserving space for a null terminator.
 */
class WriteTracker {
  std::size_t m_capacity;
  std::size_t m_total_written{0};

 public:
  /**
   * @brief Initializes the write tracker with a maximum capacity.
   *
   * @param max_capacity Total size of the underlying buffer in bytes.
   */
  constexpr explicit WriteTracker(const std::size_t max_capacity) noexcept
      : m_capacity(max_capacity) {}

  /**
   * @brief Retrieves the maximum number of bytes that can be written, excluding
   * the null terminator.
   *
   * @return The maximum writable bytes.
   */
  [[nodiscard]] constexpr std::size_t writable_capacity() const noexcept {
    return m_capacity > 0 ? m_capacity - 1 : 0;
  }

  /**
   * @brief Retrieves the current write offset within the safe boundaries.
   *
   * @return The safe write offset index.
   */
  [[nodiscard]] constexpr std::size_t current_offset() const noexcept {
    return std::min(m_total_written, writable_capacity());
  }

  /**
   * @brief Retrieves the remaining space available for safe writes.
   *
   * @return Number of writable bytes left.
   */
  [[nodiscard]] constexpr std::size_t available_space() const noexcept {
    return writable_capacity() - current_offset();
  }

  /**
   * @brief Records a write operation into the tracking metrics.
   *
   * @param n The number of bytes attempted to write.
   */
  constexpr void record_write(const std::size_t n) noexcept {
    m_total_written += n;
  }

  /**
   * @brief Retrieves the total number of bytes attempted to be written.
   *
   * @return The total written count.
   */
  [[nodiscard]] constexpr std::size_t total_written() const noexcept {
    return m_total_written;
  }
};

/**
 * @brief A non-owning, bounds-checked string buffer for formatted output.
 *
 * @note Ensures null-termination (if capacity allows) and guarantees zero
 * buffer overflows.
 */
class StringBuffer {
  std::span<char> m_buffer;
  WriteTracker m_tracker;

 public:
  /**
   * @brief Constructs a string buffer taking ownership of a provided span view.
   *
   * @param buffer The character span representing the memory to write to.
   */
  constexpr explicit StringBuffer(const std::span<char> buffer) noexcept
      : m_buffer(buffer), m_tracker(buffer.size()) {}

  /**
   * @brief Constructs a string buffer from a raw pointer and size limit.
   *
   * @param buf Pointer to the target character array.
   * @param max_size The total bounds-capacity of the array.
   */
  constexpr StringBuffer(char* buf, const std::size_t max_size) noexcept
      : m_buffer(buf, max_size), m_tracker(max_size) {}

  /**
   * @brief Appends a single character to the buffer.
   *
   * @param c The character to append.
   *
   * @note Drops the character if the buffer is full, but still increments the
   * tracked write count.
   */
  [[gnu::always_inline]]
  constexpr void push(const char c) noexcept {
    if (m_tracker.available_space() > 0)
      m_buffer[m_tracker.current_offset()] = c;

    m_tracker.record_write(1);
  }

  /**
   * @brief Appends a string view to the buffer.
   *
   * @param sv The string view containing the characters to append.
   *
   * @note If the string view is larger than the available capacity, it will be
   * truncated. The full length of the input is always tracked.
   */
  constexpr void append(const std::string_view sv) noexcept {
    const std::size_t copy_len =
        std::min(sv.length(), m_tracker.available_space());

    if (copy_len > 0)
      std::ranges::copy_n(sv.begin(), copy_len,
                          m_buffer.begin() + m_tracker.current_offset());

    m_tracker.record_write(sv.length());
  }

  /**
   *
   */
  constexpr void append_fill(const std::size_t count, const char c) noexcept {
    const std::size_t fill_len = std::min(count, m_tracker.available_space());
    if (fill_len > 0)
      std::ranges::fill_n(m_buffer.begin() + m_tracker.current_offset(),
                          fill_len, c);

    m_tracker.record_write(count);
  }

  /**
   * @brief Finalizes the buffer string by appending a null-terminator.
   *
   * @return The total number of bytes that were attempted to be written
   * throughout the buffer's lifetime.
   *
   * @note Intentionally non-const as it actively mutates the underlying memory
   * buffer to seal it.
   */
  constexpr std::size_t finish() noexcept {
    if (!m_buffer.empty()) m_buffer[m_tracker.current_offset()] = '\0';

    return m_tracker.total_written();
  }

  /**
   * @brief Requests a contiguous block of memory from the buffer for manual
   * writing.
   *
   * @param n The number of bytes requested.
   * @return A bounded span representing the allocated block, or an empty span
   * if insufficient space exists.
   *
   * @note Successfully allocated blocks will immediately advance the internal
   * write tracker. Callers must exclusively write into the returned bounds.
   */
  constexpr std::span<char> advance(const std::size_t n) noexcept {
    if (n <= m_tracker.available_space()) {
      std::span<char> block = m_buffer.subspan(m_tracker.current_offset(), n);
      m_tracker.record_write(n);
      return block;
    }

    return {};
  }

  /**
   * @brief Retrieves a mutable view of the successfully written characters so
   * far.
   *
   * @return A bounded span representing the currently active payload in the
   * buffer.
   */
  [[nodiscard]] constexpr std::span<char> active_span() noexcept {
    return m_buffer.first(m_tracker.current_offset());
  }

  /**
   * @brief Retrieves a read-only view of the successfully written characters so
   * far.
   *
   * @return A constant bounded span representing the active payload in the
   * buffer.
   */
  [[nodiscard]] constexpr std::span<const char> active_span() const noexcept {
    return m_buffer.first(m_tracker.current_offset());
  }

  /**
   * @brief Retrieves a standard string_view of the written characters.
   *
   * @return A string_view representing the active payload.
   */
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view(m_buffer.data(), m_tracker.current_offset());
  }
};
}  // namespace klib
