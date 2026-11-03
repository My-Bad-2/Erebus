#include "../include/string.hpp"
#include "string.h"

namespace klib {
// The same overlapping-stores scheme as memcpy(), then `rep stosb` beyond 64 bytes.
void* memset(void* dest, const int ch, const size_t count) noexcept {
  using string::unaligned_store;

  const auto byte = static_cast<std::uint8_t>(ch);
  const std::uint64_t v = string::broadcast_byte(byte);
  const auto base = reinterpret_cast<std::uintptr_t>(dest);

  const auto store = [base]<typename T>(const T val, const std::size_t offset) {
    unaligned_store<T>(reinterpret_cast<void*>(base + offset), val);
  };

  if (count <= 16) [[likely]] {
    if (count >= 8) {
      store(v, 0);
      store(v, count - 8);
    } else if (count >= 4) {
      store(static_cast<std::uint32_t>(v), 0);
      store(static_cast<std::uint32_t>(v), count - 4);
    } else if (count >= 2) {
      store(static_cast<std::uint16_t>(v), 0);
      store(static_cast<std::uint16_t>(v), count - 2);
    } else if (count == 1) {
      store(byte, 0);
    }

    return dest;
  }

  if (count <= 32) {
    store(v, 0);
    store(v, 8);
    store(v, count - 16);
    store(v, count - 8);
    return dest;
  }

  if (count <= 64) {
    store(v, 0);
    store(v, 8);
    store(v, 16);
    store(v, 24);
    store(v, count - 32);
    store(v, count - 24);
    store(v, count - 16);
    store(v, count - 8);
    return dest;
  }

  string::rep_stosb(dest, byte, count);
  return dest;
}
}  // namespace klib
