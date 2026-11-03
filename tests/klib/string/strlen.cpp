#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace kernel::testing::strings {
namespace {
alignas(64) char g_buffer[BUFFER_BYTES];

void fill_chars(char* chars, const std::size_t count, const char c) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    chars[i] = c;
  }
}
}  // namespace

KTEST(Strlen, measures_every_length_and_alignment) {
  for (const std::size_t length : SIZES) {
    for (std::size_t align = 0; align < 8; ++align) {
      fill_chars(g_buffer, BUFFER_BYTES, 'z');
      char* str = g_buffer + 64 + align;
      fill_chars(str, length, 'a');
      str[length] = '\0';
      KREQUIRE_EQ(klib::strlen(str), length);
    }
  }
}

// The bytes before a string share its first word. A zero among them borrowed into the first
// byte of the string in the word-at-a-time check, which made a leading 0x01 look like a
// terminator.
KTEST(Strlen, ignores_the_bytes_before_the_string) {
  for (std::size_t offset = 1; offset < 8; ++offset) {
    char* base = g_buffer + 128;
    fill_chars(base, 32, 'z');
    char* str = base + offset;
    str[0] = '\x01';
    str[1] = '\x01';
    str[2] = 'a';
    str[3] = '\0';
    base[offset - 1] = '\0';

    KEXPECT_EQ(klib::strlen(str), 3u);
    KEXPECT_EQ(klib::strnlen(str, 2), 2u);
  }
}

KTEST(Strnlen, stops_at_the_limit) {
  for (const std::size_t length : SIZES) {
    for (std::size_t align = 0; align < 8; align += 3) {
      fill_chars(g_buffer, BUFFER_BYTES, 'z');
      char* str = g_buffer + 64 + align;
      fill_chars(str, length, 'a');
      str[length] = '\0';

      for (const std::size_t limit : {std::size_t{0}, length / 2, length, length + 1, length + 9}) {
        KREQUIRE_EQ(klib::strnlen(str, limit), limit < length ? limit : length);
      }
    }
  }
}
}  // namespace kernel::testing::strings
