#include "../include/string.hpp"
#include "string.h"
#include <bitset>

namespace klib {
void *memset(void *dest, const int ch, const size_t count) noexcept {
  using namespace string;

  if (count == 0) [[unlikely]] {
    return dest;
  }

  [[assume(count > 0)]];

  const auto byte_val = static_cast<std::uint8_t>(ch);
  const std::uint64_t v8 = broadcast_byte(byte_val);

  const auto base = reinterpret_cast<std::uintptr_t>(dest); // NOLINT(*-pro-type-reinterpret-cast)

  auto store = [base]<typename T>(T val, const std::size_t offset) {
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    unaligned_store<T>(reinterpret_cast<void *>(base + offset), val);
  };

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      store(v8, 0);
      store(v8, count - 8);
    } else if (count >= 4) {
      store(static_cast<std::uint32_t>(v8), 0);
      store(static_cast<std::uint32_t>(v8), count - 4);
    } else if (count >= 2) {
      store(static_cast<std::uint16_t>(v8), 0);
      store(static_cast<std::uint16_t>(v8), count - 2);
    } else {
      store(byte_val, 0);
    }

    return dest;
  }

  if (count <= 64) {
    if (count <= 32) {
      store(v8, 0);
      store(v8, 8);
      store(v8, count - 16);
      store(v8, count - 8);
    } else {
      store(v8, 0);
      store(v8, 8);
      store(v8, 16);
      store(v8, 24);
      store(v8, count - 32);
      store(v8, count - 24);
      store(v8, count - 16);
      store(v8, count - 8);
    }

    return dest;
  }

  hw_rep_stosb(dest, byte_val, count);
  return dest;
}
} // namespace klib
