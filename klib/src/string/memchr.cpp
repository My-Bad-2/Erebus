#include "../include/string.hpp"
#include "string.h"

namespace klib {
namespace {
template <typename T, typename... Offsets>
[[gnu::always_inline]]
void *find_match(std::uintptr_t base, std::uint64_t broadcast,
                 Offsets... offsets) noexcept {
  void *match = nullptr;

  // Expands to: ((match = check(off1)) != nullptr) || ((match =
  // check(off2)) != nullptr) ...
  (void)(((match = string::check_chunk_offset<T>(
               base, static_cast<std::size_t>(offsets), broadcast)) !=
          nullptr) ||
         ...);

  return match;
}
} // namespace

void *memchr(const void *ptr, const int ch, size_t count) noexcept {
  using namespace string;
  if (count == 0) [[unlikely]] {
    return nullptr;
  }

  [[assume(count > 0)]];

  const auto p = reinterpret_cast<std::uintptr_t>(ptr);
  const std::uint64_t v8 = broadcast_byte(static_cast<std::uint8_t>(ch));

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      return find_match<std::uint64_t>(p, v8, 0, count - 8);
    }

    if (count >= 4) {
      return find_match<std::uint32_t>(p, v8, 0, count - 4);
    }

    if (count >= 2) {
      return find_match<std::uint16_t>(p, v8, 0, count - 2);
    }

    return find_match<std::uint8_t>(p, v8, 0);
  }

  if (count <= 32) {
    return find_match<std::uint64_t>(p, v8, 0, 8, count - 16, count - 8);
  }

  std::size_t offset = 0;
  while (count - offset >= 32) {
    if (void *res = find_match<std::uint64_t>(p, v8, offset, offset + 8,
                                              offset + 16, offset + 24)) {
      return res;
    }

    offset += 32;
  }

  if (offset < count) {
    const std::size_t tail = count - 32;
    return find_match<std::uint64_t>(p, v8, tail, tail + 8, tail + 16,
                                     tail + 24);
  }

  return nullptr;
}
} // namespace klib
