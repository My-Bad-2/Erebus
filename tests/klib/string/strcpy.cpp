#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
char g_dest[64];

void reset() noexcept {
  for (char& c : g_dest) {
    c = static_cast<char>(SENTINEL);
  }
}
}  // namespace

KTEST(Strcpy, copies_the_terminator_and_nothing_more) {
  reset();
  KEXPECT_EQ(klib::strcpy(g_dest, "Erebus OS"), g_dest);
  KEXPECT_STREQ(g_dest, "Erebus OS");
  KEXPECT_EQ(g_dest[10], static_cast<char>(SENTINEL));

  reset();
  klib::strcpy(g_dest, "");
  KEXPECT_EQ(g_dest[0], '\0');
  KEXPECT_EQ(g_dest[1], static_cast<char>(SENTINEL));
}

KTEST(Strncpy, pads_with_zeros) {
  reset();
  KEXPECT_EQ(klib::strncpy(g_dest, "Kernel", 16), g_dest);
  KEXPECT_STREQ(g_dest, "Kernel");
  for (std::size_t i = 6; i < 16; ++i) {
    KEXPECT_EQ(g_dest[i], '\0');
  }
  KEXPECT_EQ(g_dest[16], static_cast<char>(SENTINEL));
}

KTEST(Strncpy, truncates_without_a_terminator) {
  reset();
  klib::strncpy(g_dest, "Erebus Kernel", 6);
  KEXPECT_EQ(g_dest[5], 's');
  KEXPECT_EQ(g_dest[6], static_cast<char>(SENTINEL));

  reset();
  KEXPECT_EQ(klib::strncpy(g_dest, "Kernel", 0), g_dest);
  KEXPECT_EQ(g_dest[0], static_cast<char>(SENTINEL));
}
}  // namespace ktest::strings
