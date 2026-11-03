#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace kernel::testing::strings {
namespace {
// Room for the largest move shifted by more than its length either way.
constexpr std::size_t MOVE_BYTES = 2 * BUFFER_BYTES;
alignas(64) std::uint8_t g_buffer[MOVE_BYTES];
alignas(64) std::uint8_t g_expected[MOVE_BYTES];
alignas(64) std::uint8_t g_scratch[MOVE_BYTES];

// What memmove() must do: copy through a buffer of its own, so the overlap cannot matter.
void reference_move(std::uint8_t* base, const std::size_t dest, const std::size_t src,
                    const std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    g_scratch[i] = base[src + i];
  }
  for (std::size_t i = 0; i < count; ++i) {
    base[dest + i] = g_scratch[i];
  }
}
}  // namespace

KTEST(Memmove, handles_every_overlap) {
  constexpr std::size_t SRC_BASE = MOVE_BYTES / 2;
  constexpr std::size_t ALIGNS[] = {0, 1, 3, 7};

  for (const std::size_t count : SIZES) {
    // Destination far before, just before, on, just after and far after the source.
    const long shifts[] = {-static_cast<long>(count) - 16, -static_cast<long>(count) / 2, -1, 0, 1,
                           static_cast<long>(count) / 2,   static_cast<long>(count) + 16};

    for (const long shift : shifts) {
      for (const std::size_t src_align : ALIGNS) {
        for (const std::size_t dest_align : ALIGNS) {
          const std::size_t src = SRC_BASE + src_align;
          const std::size_t dest =
              static_cast<std::size_t>(static_cast<long>(SRC_BASE) + shift) + dest_align;

          fill_pattern(g_buffer, MOVE_BYTES, 3);
          fill_pattern(g_expected, MOVE_BYTES, 3);
          reference_move(g_expected, dest, src, count);

          void* ret = klib::memmove(g_buffer + dest, g_buffer + src, count);
          KREQUIRE_EQ(ret, static_cast<void*>(g_buffer + dest));
          KREQUIRE(same(g_buffer, g_expected, MOVE_BYTES));
        }
      }
    }
  }
}

KTEST(Memmove, same_pointer_and_zero_bytes_change_nothing) {
  fill_pattern(g_buffer, 256, 5);
  fill_pattern(g_expected, 256, 5);

  KEXPECT_EQ(klib::memmove(g_buffer + 100, g_buffer + 100, 64), static_cast<void*>(g_buffer + 100));
  KEXPECT_EQ(klib::memmove(g_buffer + 10, g_buffer + 20, 0), static_cast<void*>(g_buffer + 10));
  KEXPECT(same(g_buffer, g_expected, 256));
}
}  // namespace kernel::testing::strings
