#include "../include/string.hpp"
#include "string.h"

namespace klib {
namespace {
// memcmp()'s answer for the first differing `T` among the offsets, which go from low to high.
template <typename T, typename... Offsets>
[[gnu::always_inline]] int compare_chunks(std::uintptr_t p1, std::uintptr_t p2,
                                          Offsets... offsets) noexcept {
  int diff = 0;
  (void)(((diff = string::check_chunk<T>(p1, p2, static_cast<std::size_t>(offsets))) != 0) || ...);
  return diff;
}
}  // namespace

int memcmp(const void* s1, const void* s2, std::size_t count) noexcept {
  if (count == 0 || s1 == s2) [[unlikely]] {
    return 0;
  }

  const auto p1 = reinterpret_cast<std::uintptr_t>(s1);
  const auto p2 = reinterpret_cast<std::uintptr_t>(s2);

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      return compare_chunks<std::uint64_t>(p1, p2, 0, count - 8);
    }

    if (count >= 4) {
      return compare_chunks<std::uint32_t>(p1, p2, 0, count - 4);
    }

    if (count >= 2) {
      return compare_chunks<std::uint16_t>(p1, p2, 0, count - 2);
    }

    return compare_chunks<std::uint8_t>(p1, p2, 0);
  }

  if (count <= 32) {
    return compare_chunks<std::uint64_t>(p1, p2, 0, 8, count - 16, count - 8);
  }

  std::size_t offset = 0;
  for (; count - offset >= 32; offset += 32) {
    if (const int diff =
            compare_chunks<std::uint64_t>(p1, p2, offset, offset + 8, offset + 16, offset + 24)) {
      return diff;
    }
  }

  // The last 32 bytes, overlapping what was already compared.
  if (offset < count) {
    const std::size_t tail = count - 32;
    return compare_chunks<std::uint64_t>(p1, p2, tail, tail + 8, tail + 16, tail + 24);
  }

  return 0;
}
}  // namespace klib
