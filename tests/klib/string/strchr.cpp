#include <string.h>

#include "buffers.hpp"
#include "ktest.hpp"

namespace kernel::testing::strings {
namespace {
alignas(64) char g_buffer[BUFFER_BYTES];

// The obvious, byte at a time versions, to hold the real ones up against.
const char* first_of(const char* str, const char ch) noexcept {
  for (;; ++str) {
    if (*str == ch) {
      return str;
    }
    if (*str == '\0') {
      return nullptr;
    }
  }
}

const char* last_of(const char* str, const char ch) noexcept {
  const char* found = nullptr;
  for (;; ++str) {
    if (*str == ch) {
      found = str;
    }
    if (*str == '\0') {
      return found;
    }
  }
}
}  // namespace

KTEST(Strchr, finds_characters_and_the_terminator) {
  char str[] = "Erebus kernel";
  KEXPECT_EQ(klib::strchr(str, 'k'), str + 7);
  KEXPECT_EQ(klib::strchr(str, 'e'), str + 2);
  KEXPECT_EQ(klib::strchr(str, 'Z'), nullptr);
  KEXPECT_EQ(klib::strchr(str, '\0'), str + 13);
  KEXPECT_EQ(klib::strrchr(str, 'e'), str + 11);
  KEXPECT_EQ(klib::strrchr(str, 'E'), str);
  KEXPECT_EQ(klib::strrchr(str, 'Z'), nullptr);
  KEXPECT_EQ(klib::strrchr(str, '\0'), str + 13);
}

// A byte before the string that matches, or is zero, borrowed into the first byte of the
// string, which made `ch ^ 1` look like a match and 0x01 like a terminator.
KTEST(Strchr, ignores_the_bytes_before_the_string) {
  for (std::size_t offset = 1; offset < 8; ++offset) {
    char* base = g_buffer + 128;
    char* str = base + offset;

    base[offset - 1] = 'a';
    klib::strcpy(str, "`bcd");
    KEXPECT_EQ(klib::strchr(str, 'a'), nullptr);

    base[offset - 1] = '\0';
    klib::strcpy(str, "\x01x");
    KEXPECT_EQ(klib::strchr(str, 'x'), str + 1);
  }
}

// A byte equal to `ch ^ 1` right after a match used to be taken for the last match.
KTEST(Strrchr, is_not_fooled_by_a_neighbour_of_the_match) {
  char* str = g_buffer + 128;
  klib::strcpy(str, "xxxxxxbcxxxxxxxxx");
  KEXPECT_EQ(klib::strrchr(str, 'b'), str + 6);
}

KTEST(Strchr, agrees_with_a_plain_loop_on_random_strings) {
  Rng rng{1234};
  constexpr char ALPHABET[] = {'`', 'a', 'b', 'c', '\x01'};
  constexpr char WANTED[] = {'`', 'a', 'b', 'c', '\x01', '\0'};

  for (std::uint32_t round = 0; round < 2000; ++round) {
    char* str = g_buffer + 128 + round % 8;
    const std::size_t length = round % 41;
    for (std::size_t i = 0; i < length; ++i) {
      str[i] = ALPHABET[rng.below(sizeof(ALPHABET))];
    }
    str[length] = '\0';

    for (const char ch : WANTED) {
      KREQUIRE_EQ(static_cast<const char*>(klib::strchr(str, ch)), first_of(str, ch));
      KREQUIRE_EQ(static_cast<const char*>(klib::strrchr(str, ch)), last_of(str, ch));
    }
  }
}
}  // namespace kernel::testing::strings
