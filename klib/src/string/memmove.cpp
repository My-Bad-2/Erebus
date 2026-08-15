#include "../include/string.hpp"
#include "string.h"

namespace klib {
void *memmove(void *dest, const void *src, size_t count) noexcept {
  if (count == 0 || dest == src) [[unlikely]] {
    return dest;
  }

  using namespace string;
  [[assume(count > 0)]];

  const auto d_base = reinterpret_cast<std::uintptr_t>(dest);
  const auto s_base = reinterpret_cast<std::uintptr_t>(src);

  // Forward copy (dest < src / non-overlapping)
  if (d_base - s_base >= count) [[likely]] {
    if (count <= 16) [[likely]] {
      if (count >= 8) {
        transfer<std::uint64_t>(d_base, s_base, 0, count - 8);
      } else if (count >= 4) {
        transfer<std::uint32_t>(d_base, s_base, 0, count - 4);
      } else if (count >= 2) {
        transfer<std::uint16_t>(d_base, s_base, 0, count - 2);
      } else {
        transfer<std::uint8_t>(d_base, s_base, 0);
      }

      return dest;
    }

    if (count <= 64) {
      if (count <= 32) {
        transfer<std::uint64_t>(d_base, s_base, 0, 8, count - 16, count - 8);
      } else {
        transfer<std::uint64_t>(d_base, s_base, 0, 8, 16, 24, count - 32, count - 24, count - 16, count - 8);
      }

      return dest;
    }

    hw_rep_movsb(dest, src, count);
    return dest;
  }

  // Backward Copy (dest > src and regions overlap)
  if (count <= 64) {
    if (count <= 16) {
      if (count >= 8) {
        transfer<std::uint64_t>(d_base, s_base, count - 8, 0);
      } else if (count >= 4) {
        transfer<std::uint32_t>(d_base, s_base, count - 4, 0);
      } else if (count >= 2) {
        transfer<std::uint16_t>(d_base, s_base, count - 2, 0);
      } else {
        transfer<std::uint8_t>(d_base, s_base, 0);
      }

      return dest;
    }

    if (count <= 32) {
      transfer<std::uint64_t>(d_base, s_base, count - 8, count - 16, 8, 0);
      return dest;
    }

    // 33-64 bytes
    transfer<std::uint64_t>(d_base, s_base, count - 8, count - 16, count - 24, count - 32, 24, 16, 8, 0);
    return dest;
  }

  // Backward Loop (> 64 bytes)
  std::size_t rem = count;

  while (rem >= 64) { // NOLINT(*-id-dependent-backward-branch)
    rem -= 64;
    transfer<std::uint64_t>(d_base, s_base, rem + 56, rem + 48, rem + 40, rem + 32, rem + 24, rem + 16, rem + 8,
                            rem + 0);
  }

  // Process remaining tail
  if (rem > 0) {
    if (rem <= 16) {
      if (rem >= 8) {
        transfer<std::uint64_t>(d_base, s_base, rem - 8, 0);
      } else if (rem >= 4) {
        transfer<std::uint32_t>(d_base, s_base, rem - 4, 0);
      } else if (rem >= 2) {
        transfer<std::uint16_t>(d_base, s_base, rem - 2, 0);
      } else {
        transfer<std::uint8_t>(d_base, s_base, 0);
      }
    } else if (rem <= 32) {
      transfer<std::uint64_t>(d_base, s_base, rem - 8, rem - 16, 8, 0);
    } else {
      transfer<std::uint64_t>(d_base, s_base, rem - 8, rem - 16, rem - 24, rem - 32, 24, 16, 8, 0);
    }
  }

  return dest;
}
} // namespace klib
