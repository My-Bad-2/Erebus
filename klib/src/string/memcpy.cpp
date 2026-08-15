#include "../include/string.hpp"
#include "string.h"

namespace klib {
void *memcpy(void *__restrict dest, const void *__restrict src, size_t count) noexcept {
  using namespace string;

  if (count == 0) [[unlikely]] {
    return dest;
  }

  [[assume(count > 0)]];

  const auto d_base = reinterpret_cast<std::uintptr_t>(dest);
  const auto s_base = reinterpret_cast<std::uintptr_t>(src);

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      copy_chunk<std::uint64_t>(d_base, s_base, 0);
      copy_chunk<std::uint64_t>(d_base, s_base, count - 8);
    } else if (count >= 4) {
      copy_chunk<std::uint32_t>(d_base, s_base, 0);
      copy_chunk<std::uint32_t>(d_base, s_base, count - 4);
    } else if (count >= 2) {
      copy_chunk<std::uint16_t>(d_base, s_base, 0);
      copy_chunk<std::uint16_t>(d_base, s_base, count - 2);
    } else {
      copy_chunk<std::uint8_t>(d_base, s_base, 0);
    }

    return dest;
  }

  if (count <= 64) {
    if (count <= 32) {
      // Front 16 bytes
      copy_chunk<std::uint64_t>(d_base, s_base, 0);
      copy_chunk<std::uint64_t>(d_base, s_base, 8);

      // Back 16 bytes
      copy_chunk<std::uint64_t>(d_base, s_base, count - 16);
      copy_chunk<std::uint64_t>(d_base, s_base, count - 8);
    } else {
      // Front 32 bytes
      copy_chunk<std::uint64_t>(d_base, s_base, 0);
      copy_chunk<std::uint64_t>(d_base, s_base, 8);
      copy_chunk<std::uint64_t>(d_base, s_base, 16);
      copy_chunk<std::uint64_t>(d_base, s_base, 24);

      // Back 32 bytes
      copy_chunk<std::uint64_t>(d_base, s_base, count - 32);
      copy_chunk<std::uint64_t>(d_base, s_base, count - 24);
      copy_chunk<std::uint64_t>(d_base, s_base, count - 16);
      copy_chunk<std::uint64_t>(d_base, s_base, count - 8);
    }

    return dest;
  }

  hw_rep_movsb(dest, src, count);
  return dest;
}
} // namespace klib
