#include "../include/string.hpp"
#include "string.h"

namespace klib {
namespace {
// The first match among one `T` at each offset, checked in order. Later chunks may overlap
// earlier ones; a match in the overlap would already have been found.
template <typename T, typename... Offsets>
[[gnu::always_inline]] inline void* find_first(const std::uintptr_t base,
                                               const std::uint64_t pattern,
                                               const Offsets... offsets) noexcept {
  void* match = nullptr;
  (void)(((match = string::find_in_chunk<T>(base, static_cast<std::size_t>(offsets), pattern)) !=
          nullptr) ||
         ...);
  return match;
}
}  // namespace

void* memchr(const void* ptr, const int ch, const size_t count) noexcept {
  using string::broadcast_byte;

  if (count == 0) [[unlikely]] {
    return nullptr;
  }

  const auto p = reinterpret_cast<std::uintptr_t>(ptr);
  const std::uint64_t pattern = broadcast_byte(static_cast<std::uint8_t>(ch));

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      return find_first<std::uint64_t>(p, pattern, 0, count - 8);
    }

    if (count >= 4) {
      return find_first<std::uint32_t>(p, pattern, 0, count - 4);
    }

    if (count >= 2) {
      return find_first<std::uint16_t>(p, pattern, 0, count - 2);
    }

    return find_first<std::uint8_t>(p, pattern, 0);
  }

  if (count <= 32) {
    return find_first<std::uint64_t>(p, pattern, 0, 8, count - 16, count - 8);
  }

  std::size_t offset = 0;
  for (; count - offset >= 32; offset += 32) {
    if (void* match =
            find_first<std::uint64_t>(p, pattern, offset, offset + 8, offset + 16, offset + 24)) {
      return match;
    }
  }

  // The last 32 bytes, overlapping what was already searched.
  if (offset < count) {
    const std::size_t tail = count - 32;
    return find_first<std::uint64_t>(p, pattern, tail, tail + 8, tail + 16, tail + 24);
  }

  return nullptr;
}
}  // namespace klib
