#include <limits>

#include "../include/string.hpp"
#include "string.h"

namespace klib {
// Compares a word at a time while neither string is near the end of a page, so that a word read
// past a terminator cannot fault; byte by byte across page ends and for the last few bytes.
int strncmp(const char* s1, const char* s2, size_t count) noexcept {
  using string::has_zero_byte;
  using string::unaligned_load;

  if (count == 0 || s1 == s2) [[unlikely]] {
    return 0;
  }

  auto p1 = reinterpret_cast<std::uintptr_t>(s1);
  auto p2 = reinterpret_cast<std::uintptr_t>(s2);

  while (count >= 8) {
    if (((p1 & 4095) > 4088) || ((p2 & 4095) > 4088)) [[unlikely]] {
      const auto c1 = *reinterpret_cast<const std::uint8_t*>(p1);
      const auto c2 = *reinterpret_cast<const std::uint8_t*>(p2);

      if (c1 != c2 || c1 == '\0') {
        return static_cast<int>(c1) - static_cast<int>(c2);
      }

      ++p1;
      ++p2;
      --count;
      continue;
    }

    const auto v1 = unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(p1));
    const auto v2 = unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(p2));

    const std::uint64_t diff = v1 ^ v2;
    const std::uint64_t nulls = has_zero_byte<std::uint64_t>(v1);

    if (diff == 0 && nulls == 0) [[likely]] {
      p1 += 8;
      p2 += 8;
      count -= 8;
      continue;
    }

    // Equal up to a terminator: the strings end together.
    if (diff == 0) {
      return 0;
    }

    // A difference, and maybe a terminator before it. Only the lowest flagged byte of `nulls`
    // is exact, and only the lowest is used.
    const auto diff_shift = static_cast<std::uint32_t>(std::countr_zero(diff)) & ~7U;
    if (nulls != 0 && (static_cast<std::uint32_t>(std::countr_zero(nulls)) & ~7U) < diff_shift) {
      return 0;
    }

    const int byte1 = static_cast<std::uint8_t>((v1 >> diff_shift) & 0xFF);
    const int byte2 = static_cast<std::uint8_t>((v2 >> diff_shift) & 0xFF);

    return byte1 - byte2;
  }

  while (count > 0) {
    const auto c1 = *reinterpret_cast<const std::uint8_t*>(p1);
    const auto c2 = *reinterpret_cast<const std::uint8_t*>(p2);

    if (c1 != c2 || c1 == '\0') {
      return static_cast<int>(c1) - static_cast<int>(c2);
    }

    ++p1;
    ++p2;
    --count;
  }

  return 0;
}

int strcmp(const char* s1, const char* s2) noexcept {
  return strncmp(s1, s2, std::numeric_limits<std::size_t>::max());
}
}  // namespace klib
