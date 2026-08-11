#include "../include/string.hpp"
#include "string.h"

namespace klib {
int memcmp(const void *s1, const void *s2, size_t count) noexcept {
  if (count == 0 || s1 == s2) [[unlikely]] {
    return 0;
  }

  using namespace string;

  [[assume(count > 0)]];

  const auto p1 = reinterpret_cast<std::uintptr_t>(s1);
  const auto p2 = reinterpret_cast<std::uintptr_t>(s2);

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      if (const int d = check_chunk<std::uint64_t>(p1, p2, 0)) {
        return d;
      }

      return check_chunk<std::uint64_t>(p1, p2, count - 8);
    }

    if (count >= 4) {
      if (const int d = check_chunk<std::uint32_t>(p1, p2, 0)) {
        return d;
      }

      return check_chunk<std::uint32_t>(p1, p2, count - 4);
    }

    if (count >= 2) {
      if (const int d = check_chunk<std::uint16_t>(p1, p2, 0)) {
        return d;
      }

      return check_chunk<std::uint16_t>(p1, p2, count - 2);
    }

    return check_chunk<std::uint8_t>(p1, p2, 0);
  }

  if (count <= 32) {
    if (const int d = check_chunk<std::uint64_t>(p1, p2, 0)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, 8)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, count - 16)) {
      return d;
    }

    return check_chunk<std::uint64_t>(p1, p2, count - 8);
  }

  std::size_t offset = 0;
  while (count - offset >= 32) {
    if (const int d = check_chunk<std::uint64_t>(p1, p2, offset + 0)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, offset + 8)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, offset + 16)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, offset + 24)) {
      return d;
    }

    offset += 32;
  }

  if (offset < count) {
    const std::size_t tail = count - 32;

    if (const int d = check_chunk<std::uint64_t>(p1, p2, tail + 0)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, tail + 8)) {
      return d;
    }

    if (const int d = check_chunk<std::uint64_t>(p1, p2, tail + 16)) {
      return d;
    }

    return check_chunk<std::uint64_t>(p1, p2, tail + 24);
  }

  return 0;
}
} // namespace klib
