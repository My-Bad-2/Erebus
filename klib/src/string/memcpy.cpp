#include "../include/string.hpp"
#include "string.h"

namespace klib {
// Up to 64 bytes: two (or four, or eight) loads and stores from either end, overlapping in the
// middle, which covers every length in a band without a loop or a byte tail. Beyond that,
// `rep movsb`, which is the fastest copy on CPUs with ERMS/FSRM.
void* memcpy(void* __restrict dest, const void* __restrict src, const size_t count) noexcept {
  using string::copy_chunk;

  const auto d = reinterpret_cast<std::uintptr_t>(dest);
  const auto s = reinterpret_cast<std::uintptr_t>(src);

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      copy_chunk<std::uint64_t>(d, s, 0);
      copy_chunk<std::uint64_t>(d, s, count - 8);
    } else if (count >= 4) {
      copy_chunk<std::uint32_t>(d, s, 0);
      copy_chunk<std::uint32_t>(d, s, count - 4);
    } else if (count >= 2) {
      copy_chunk<std::uint16_t>(d, s, 0);
      copy_chunk<std::uint16_t>(d, s, count - 2);
    } else if (count == 1) {
      copy_chunk<std::uint8_t>(d, s, 0);
    }

    return dest;
  }

  if (count <= 32) {
    copy_chunk<std::uint64_t>(d, s, 0);
    copy_chunk<std::uint64_t>(d, s, 8);
    copy_chunk<std::uint64_t>(d, s, count - 16);
    copy_chunk<std::uint64_t>(d, s, count - 8);
    return dest;
  }

  if (count <= 64) {
    copy_chunk<std::uint64_t>(d, s, 0);
    copy_chunk<std::uint64_t>(d, s, 8);
    copy_chunk<std::uint64_t>(d, s, 16);
    copy_chunk<std::uint64_t>(d, s, 24);
    copy_chunk<std::uint64_t>(d, s, count - 32);
    copy_chunk<std::uint64_t>(d, s, count - 24);
    copy_chunk<std::uint64_t>(d, s, count - 16);
    copy_chunk<std::uint64_t>(d, s, count - 8);
    return dest;
  }

  string::rep_movsb(dest, src, count);
  return dest;
}
}  // namespace klib
