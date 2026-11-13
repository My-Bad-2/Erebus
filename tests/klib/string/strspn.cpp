#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
alignas(64) char g_buffer[256];
}  // namespace

KTEST(Strspn, counts_the_leading_accepted_characters) {
  const char* str = "742 Evergreen";
  KEXPECT_EQ(klib::strspn(str, "0123456789"), 3u);
  KEXPECT_EQ(klib::strspn(str, " 742"), 4u);
  KEXPECT_EQ(klib::strspn(str, "Evergreen"), 0u);
  KEXPECT_EQ(klib::strspn(str, ""), 0u);
  KEXPECT_EQ(klib::strspn("", "Kernel"), 0u);
}

KTEST(Strcspn, counts_the_leading_rejected_characters) {
  const char* str = "kernel.config.bin";
  KEXPECT_EQ(klib::strcspn(str, "."), 6u);
  KEXPECT_EQ(klib::strcspn(str, "f"), 10u);
  KEXPECT_EQ(klib::strcspn(str, "Z"), 17u);
  KEXPECT_EQ(klib::strcspn(str, ""), 17u);
  KEXPECT_EQ(klib::strcspn("", "Kernel"), 0u);
}

KTEST(Strspn, stops_at_every_position_and_alignment) {
  for (std::size_t align = 0; align < 8; ++align) {
    for (std::size_t at = 0; at < 64; ++at) {
      char* str = g_buffer + align;
      for (std::size_t i = 0; i < 80; ++i) {
        str[i] = i % 2 == 0 ? 'A' : 'B';
      }
      str[80] = '\0';
      str[at] = 'Z';

      KREQUIRE_EQ(klib::strspn(str, "AB"), at);
      KREQUIRE_EQ(klib::strcspn(str, "YZ"), at);
    }
  }
}

// Bytes above 0x7F are set members like any other.
KTEST(Strspn, handles_high_bytes) {
  KEXPECT_EQ(klib::strspn("\xFF\xFE\x01", "\xFE\xFF"), 2u);
  KEXPECT_EQ(klib::strcspn("ab\x80"
                           "c",
                           "\x80"),
             2u);
}
}  // namespace ktest::strings
