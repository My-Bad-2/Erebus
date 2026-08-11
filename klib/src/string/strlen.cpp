#include "../include/string.hpp"
#include "string.h"
#include <bit>

namespace klib {
size_t strlen(const char *str) noexcept {
  return strnlen(str, std::numeric_limits<std::size_t>::max());
}

size_t strnlen(const char *str, size_t maxlen) noexcept {
  if (maxlen == 0) [[unlikely]] {
    return 0;
  }

  const auto base = reinterpret_cast<std::uintptr_t>(str);

  // Round down to the nearest 8-byte aligned cache line boundary.
  const std::uint32_t offset = base & 7ULL;
  std::uintptr_t current = base - offset;

  auto word = string::unaligned_load<std::uint64_t>(
      reinterpret_cast<const void *>(current));
  auto match = string::has_zero_byte<std::uint64_t>(word);

  // Shift out garbage bytes before the actual string pointer
  match >>= (offset * 8);

  if (match != 0) [[unlikely]] {
    const std::size_t len =
        static_cast<std::size_t>(std::countr_zero(match)) / 8;
    return len < maxlen ? len : maxlen;
  }

  current += 8;
  std::size_t processed = 8 - offset;

  // If a match is found in this block, `len` is guaranteed to be < maxlen
  // (because processed + 32 <= maxlen).
  while (processed + 32 <= maxlen) {
    const auto w1 = string::unaligned_load<std::uint64_t>(
        reinterpret_cast<const void *>(current));
    if (const auto m1 = string::has_zero_byte<std::uint64_t>(w1)) [[unlikely]] {
      return processed + (static_cast<std::size_t>(std::countr_zero(m1)) / 8);
    }

    const auto w2 = string::unaligned_load<std::uint64_t>(
        reinterpret_cast<const void *>(current + 8));
    if (const auto m2 = string::has_zero_byte<std::uint64_t>(w2)) [[unlikely]] {
      return processed + 8 +
             (static_cast<std::size_t>(std::countr_zero(m2)) / 8);
    }

    const auto w3 = string::unaligned_load<std::uint64_t>(
        reinterpret_cast<const void *>(current + 16));
    if (const auto m3 = string::has_zero_byte<std::uint64_t>(w3)) [[unlikely]] {
      return processed + 16 +
             (static_cast<std::size_t>(std::countr_zero(m3)) / 8);
    }

    const auto w4 = string::unaligned_load<std::uint64_t>(
        reinterpret_cast<const void *>(current + 24));
    if (const auto m4 = string::has_zero_byte<std::uint64_t>(w4)) [[unlikely]] {
      return processed + 24 +
             (static_cast<std::size_t>(std::countr_zero(m4)) / 8);
    }

    current += 32;
    processed += 32;
  }

  // Here, `len` could potentially exceed `maxlen`, so we safely clamp it.
  while (processed < maxlen) {
    const auto w = string::unaligned_load<std::uint64_t>(
        reinterpret_cast<const void *>(current));
    if (const auto m = string::has_zero_byte<std::uint64_t>(w)) [[unlikely]] {
      const std::size_t len =
          processed + (static_cast<std::size_t>(std::countr_zero(m)) / 8);
      return len < maxlen ? len : maxlen;
    }

    current += 8;
    processed += 8;
  }

  return maxlen;
}
} // namespace klib
