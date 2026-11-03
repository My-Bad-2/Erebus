#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace kernel::testing::strings {
namespace {
alignas(64) std::uint8_t g_src[BUFFER_BYTES];
alignas(64) std::uint8_t g_dest[BUFFER_BYTES];
alignas(64) std::uint8_t g_expected[BUFFER_BYTES];
}  // namespace

KTEST(Memcpy, copies_every_size_between_any_alignments) {
  fill_pattern(g_src, BUFFER_BYTES, 1);

  for (const std::size_t count : SIZES) {
    for (std::size_t dest_align = 0; dest_align < 8; ++dest_align) {
      for (std::size_t src_align = 0; src_align < 8; ++src_align) {
        fill(g_dest, BUFFER_BYTES, SENTINEL);
        fill(g_expected, BUFFER_BYTES, SENTINEL);
        for (std::size_t i = 0; i < count; ++i) {
          g_expected[dest_align + i] = g_src[src_align + i];
        }

        void* ret = klib::memcpy(g_dest + dest_align, g_src + src_align, count);
        KREQUIRE_EQ(ret, static_cast<void*>(g_dest + dest_align));
        // Bytes outside the destination must be untouched as well.
        KREQUIRE(same(g_dest, g_expected, BUFFER_BYTES));
      }
    }
  }
}

KTEST(Memcpy, zero_bytes_change_nothing) {
  fill(g_dest, 64, SENTINEL);
  KEXPECT_EQ(klib::memcpy(g_dest + 10, g_src, 0), static_cast<void*>(g_dest + 10));
  KEXPECT_EQ(g_dest[10], SENTINEL);
  // As in C2y, null pointers are fine when there is nothing to copy.
  KEXPECT_EQ(klib::memcpy(nullptr, nullptr, 0), nullptr);
}
}  // namespace kernel::testing::strings
