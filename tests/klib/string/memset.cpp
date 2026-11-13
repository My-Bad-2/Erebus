#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
alignas(64) std::uint8_t g_buffer[BUFFER_BYTES];
}  // namespace

KTEST(Memset, fills_every_size_and_alignment) {
  // Only the low byte of the value counts: -1 and 256 are 0xFF and 0x00.
  constexpr int VALUES[] = {0x00, 0xFF, 0x55, -1, 256};

  for (const std::size_t count : SIZES) {
    for (const int value : VALUES) {
      for (std::size_t align = 0; align < 8; ++align) {
        fill(g_buffer, BUFFER_BYTES, SENTINEL);

        void* ret = klib::memset(g_buffer + align, value, count);
        KREQUIRE_EQ(ret, static_cast<void*>(g_buffer + align));

        const auto byte = static_cast<std::uint8_t>(value);
        for (std::size_t i = 0; i < BUFFER_BYTES; ++i) {
          const bool inside = i >= align && i < align + count;
          KREQUIRE_EQ(g_buffer[i], inside ? byte : SENTINEL);
        }
      }
    }
  }
}

KTEST(Memset, zero_bytes_change_nothing) {
  fill(g_buffer, 64, SENTINEL);
  KEXPECT_EQ(klib::memset(g_buffer + 10, 0, 0), static_cast<void*>(g_buffer + 10));
  KEXPECT_EQ(g_buffer[10], SENTINEL);
}
}  // namespace ktest::strings
