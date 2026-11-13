#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
alignas(64) std::uint8_t g_buffer[BUFFER_BYTES];
}  // namespace

KTEST(Memchr, finds_the_first_match_at_every_position) {
  for (const std::size_t count : SIZES) {
    for (std::size_t align = 0; align < 8; ++align) {
      std::uint8_t* base = g_buffer + align;

      fill(g_buffer, BUFFER_BYTES, 'x');
      KREQUIRE_EQ(klib::memchr(base, 'Q', count), nullptr);

      for (std::size_t at = 0; at < count; ++at) {
        fill(g_buffer, BUFFER_BYTES, 'x');
        base[at] = 'Q';
        // A second match later on must not win.
        if (at + 1 < count) {
          base[count - 1] = 'Q';
        }
        KREQUIRE_EQ(klib::memchr(base, 'Q', count), static_cast<void*>(base + at));
      }

      // A match just past the end does not count.
      fill(g_buffer, BUFFER_BYTES, 'x');
      base[count] = 'Q';
      KREQUIRE_EQ(klib::memchr(base, 'Q', count), nullptr);
    }
  }
}

KTEST(Memchr, compares_as_unsigned_char) {
  fill(g_buffer, 64, 0);
  g_buffer[40] = 0xFF;
  KEXPECT_EQ(klib::memchr(g_buffer, -1, 64), static_cast<void*>(g_buffer + 40));
  KEXPECT_EQ(klib::memchr(g_buffer, 0x1FF, 64), static_cast<void*>(g_buffer + 40));
  KEXPECT_EQ(klib::memchr(g_buffer, 0, 64), static_cast<void*>(g_buffer));
}
}  // namespace ktest::strings
