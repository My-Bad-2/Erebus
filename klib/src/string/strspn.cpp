#include "../include/string.hpp"
#include "string.h"
#include <cstddef>
#include <cstdint>

namespace klib {
namespace {
template <bool AcceptSet> [[gnu::always_inline]] std::size_t span_impl(const char *dest, const char *src) noexcept {
  using namespace string;
  std::uint64_t map[4] = {0, 0, 0, 0};

  // For strcspn, we must stop at the null terminator. By forcing the \0 bit
  // to be part of the "reject" set, we eliminate the `if (c == '\0`)`
  // branch.
  if constexpr (!AcceptSet) {
    map[0] = 1UL;
  }

  for (const unsigned char *s = reinterpret_cast<const unsigned char *>(src); *s; ++s) {
    map[*s >> 6] |= (1UL << (*s & 63));
  }

  const auto base = reinterpret_cast<std::uintptr_t>(dest);
  std::size_t len = 0;

  const std::uint32_t offset = base & 7UL;
  std::uintptr_t current = base - offset;

  std::uint64_t w = unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(current));
  w >>= (offset * 8);

  for (std::uint32_t i = offset; i < 8; ++i) {
    const unsigned char c = w & 0xFF;
    const bool in_set = (map[c >> 6] & (1ULL << (c & 63))) != 0;

    if (in_set != AcceptSet) {
      return len;
    }

    ++len;
    w >>= 8;
  }

  current += 8;

  while (true) {
    w = string::unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(current));

    for (std::uint32_t i = 0; i < 8; ++i) {
      const unsigned char c = w & 0xFF;
      const bool in_set = (map[c >> 6] & (1ULL << (c & 63))) != 0;

      if (in_set != AcceptSet) {
        return len;
      }

      ++len;
      w >>= 8;
    }

    current += 8;
  }
}
} // namespace

size_t strspn(const char *dest, const char *src) noexcept { return span_impl<true>(dest, src); }

size_t strcspn(const char *dest, const char *src) noexcept { return span_impl<false>(dest, src); }
} // namespace klib
