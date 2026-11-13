#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace ktest::strings {
namespace {
char g_dest[64];

void reset(const char* start) noexcept {
  for (char& c : g_dest) {
    c = static_cast<char>(SENTINEL);
  }
  klib::strcpy(g_dest, start);
}
}  // namespace

KTEST(Strcat, appends_with_a_terminator) {
  reset("Erebus OS");
  KEXPECT_EQ(klib::strcat(g_dest, " Kernel"), g_dest);
  KEXPECT_STREQ(g_dest, "Erebus OS Kernel");
  KEXPECT_EQ(g_dest[17], static_cast<char>(SENTINEL));

  reset("Kernel");
  klib::strcat(g_dest, "");
  KEXPECT_STREQ(g_dest, "Kernel");
}

KTEST(Strncat, appends_at_most_count_characters_then_a_terminator) {
  reset("System");
  klib::strncat(g_dest, " Initialization", 5);
  KEXPECT_STREQ(g_dest, "System Init");
  KEXPECT_EQ(g_dest[12], static_cast<char>(SENTINEL));

  // A count beyond the source does not pad, unlike strncpy().
  reset("Hello");
  klib::strncat(g_dest, " World", 64);
  KEXPECT_STREQ(g_dest, "Hello World");
  KEXPECT_EQ(g_dest[12], static_cast<char>(SENTINEL));

  reset("Erebus");
  KEXPECT_EQ(klib::strncat(g_dest, " Kernel", 0), g_dest);
  KEXPECT_STREQ(g_dest, "Erebus");
}
}  // namespace ktest::strings
