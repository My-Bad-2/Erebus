#include <limits>

#include "../include/string.hpp"
#include "string.h"

namespace klib {
size_t strlen(const char* str) noexcept {
  return strnlen(str, std::numeric_limits<std::size_t>::max());
}

// Reads aligned words only, so it never touches a page past the one holding the terminator.
size_t strnlen(const char* str, const size_t maxlen) noexcept {
  using string::has_zero_byte;
  using string::unaligned_load;

  if (maxlen == 0) [[unlikely]] {
    return 0;
  }

  const std::uint32_t offset = string::word_offset(str);
  const auto clamp = [maxlen](const std::size_t len) { return len < maxlen ? len : maxlen; };

  // The bytes before `str` in its first word are not part of it. Treat them as 0xFF: a zero
  // among them would otherwise borrow into the first real byte and could fake a terminator.
  const std::uint64_t first = string::load_aligned_word(str) | string::low_bytes(offset);
  if (const std::uint64_t match = has_zero_byte(first)) {
    return clamp((static_cast<std::size_t>(std::countr_zero(match)) / 8) - offset);
  }

  auto current = (reinterpret_cast<std::uintptr_t>(str) & ~std::uintptr_t{7}) + 8;
  std::size_t scanned = 8 - offset;

  // Four words per round while all of them are within `maxlen`, so no clamping is needed.
  for (; scanned < maxlen && maxlen - scanned >= 32; current += 32, scanned += 32) {
    for (std::size_t i = 0; i < 4; ++i) {
      const auto w = unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(current + i * 8));
      if (const std::uint64_t match = has_zero_byte(w)) [[unlikely]] {
        return scanned + i * 8 + (static_cast<std::size_t>(std::countr_zero(match)) / 8);
      }
    }
  }

  for (; scanned < maxlen; current += 8, scanned += 8) {
    const auto w = unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(current));
    if (const std::uint64_t match = has_zero_byte(w)) [[unlikely]] {
      return clamp(scanned + (static_cast<std::size_t>(std::countr_zero(match)) / 8));
    }
  }

  return maxlen;
}
}  // namespace klib
