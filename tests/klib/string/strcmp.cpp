#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace kernel::testing::strings {
namespace {
// Two pages, so that strings can be put right against the end of the first one.
alignas(4096) char g_left[8192];
alignas(4096) char g_right[8192];

[[nodiscard]] int sign(const int value) noexcept { return (value > 0) - (value < 0); }
}  // namespace

KTEST(Strcmp, orders_strings_as_unsigned_char) {
  KEXPECT_EQ(klib::strcmp("Erebus", "Erebus"), 0);
  KEXPECT_EQ(sign(klib::strcmp("Erebus", "Erebut")), -1);
  KEXPECT_EQ(sign(klib::strcmp("Erebut", "Erebus")), 1);
  KEXPECT_EQ(sign(klib::strcmp("Ere", "Erebus")), -1);
  KEXPECT_EQ(sign(klib::strcmp("", "a")), -1);
  // 0xFF sorts after 'a' because the bytes compare as unsigned.
  KEXPECT_EQ(sign(klib::strcmp("\xFF", "a")), 1);
}

KTEST(Strncmp, stops_at_the_count_and_the_terminator) {
  KEXPECT_EQ(klib::strncmp("abc", "abd", 0), 0);
  KEXPECT_EQ(klib::strncmp("abc", "abd", 2), 0);
  KEXPECT_EQ(sign(klib::strncmp("abc", "abd", 3)), -1);
  // Nothing after a terminator counts, however long the count.
  KEXPECT_EQ(klib::strncmp("ab\0x", "ab\0y", 8), 0);
  KEXPECT_EQ(sign(klib::strncmp("ab", "abc", 64)), -1);
}

KTEST(Strncmp, finds_differences_at_every_position_and_alignment) {
  for (std::size_t length = 1; length < 70; ++length) {
    for (std::size_t align = 0; align < 8; ++align) {
      char* a = g_left + 64 + align;
      char* b = g_right + 64 + (7 - align);
      for (std::size_t i = 0; i < length; ++i) {
        a[i] = b[i] = static_cast<char>('a' + i % 26);
      }
      a[length] = b[length] = '\0';
      KREQUIRE_EQ(klib::strcmp(a, b), 0);

      const std::size_t at = length / 2;
      b[at] = static_cast<char>(a[at] + 1);
      KREQUIRE_EQ(sign(klib::strcmp(a, b)), -1);
      KREQUIRE_EQ(sign(klib::strncmp(b, a, length)), 1);
      KREQUIRE_EQ(klib::strncmp(a, b, at), 0);
    }
  }
}

// Strings ending just before a page boundary: reading a word past the terminator there would
// touch the next page. The pages here are mapped, so this checks the answers, not the faults.
KTEST(Strncmp, handles_strings_against_a_page_end) {
  for (std::size_t length = 0; length < 16; ++length) {
    char* a = g_left + 4096 - length - 1;
    char* b = g_right + 4096 - length - 1;
    for (std::size_t i = 0; i < length; ++i) {
      a[i] = b[i] = 'k';
    }
    a[length] = b[length] = '\0';

    KREQUIRE_EQ(klib::strncmp(a, b, length + 1), 0);
    KREQUIRE_EQ(klib::strcmp(a, b), 0);
  }
}
}  // namespace kernel::testing::strings
