#include <ctype.h>

#include "ktest.hpp"

namespace ktest {
namespace {
// The "C" locale's classes, written out plainly. Every function gets every unsigned char value
// and EOF; the bytes from 0x80 up and EOF belong to no class.
[[nodiscard]] constexpr bool in(const int c, const int lo, const int hi) noexcept {
  return c >= lo && c <= hi;
}

[[nodiscard]] constexpr bool lower(const int c) noexcept { return in(c, 'a', 'z'); }
[[nodiscard]] constexpr bool upper(const int c) noexcept { return in(c, 'A', 'Z'); }
[[nodiscard]] constexpr bool digit(const int c) noexcept { return in(c, '0', '9'); }
[[nodiscard]] constexpr bool alpha(const int c) noexcept { return lower(c) || upper(c); }
[[nodiscard]] constexpr bool graph(const int c) noexcept { return in(c, '!', '~'); }

constexpr int EOF_VALUE = -1;
}  // namespace

KTEST(Ctype, classifies_every_byte_like_the_c_locale) {
  for (int c = EOF_VALUE; c <= 255; ++c) {
    KREQUIRE_EQ(klib::islower(c), lower(c));
    KREQUIRE_EQ(klib::isupper(c), upper(c));
    KREQUIRE_EQ(klib::isdigit(c), digit(c));
    KREQUIRE_EQ(klib::isalpha(c), alpha(c));
    KREQUIRE_EQ(klib::isalnum(c), alpha(c) || digit(c));
    KREQUIRE_EQ(klib::isxdigit(c), digit(c) || in(c, 'a', 'f') || in(c, 'A', 'F'));
    KREQUIRE_EQ(klib::isspace(c), c == ' ' || in(c, '\t', '\r'));
    KREQUIRE_EQ(klib::isblank(c), c == ' ' || c == '\t');
    KREQUIRE_EQ(klib::isprint(c), in(c, ' ', '~'));
    KREQUIRE_EQ(klib::isgraph(c), graph(c));
    KREQUIRE_EQ(klib::iscntrl(c), in(c, 0, 0x1F) || c == 0x7F);
    KREQUIRE_EQ(klib::ispunct(c), graph(c) && !alpha(c) && !digit(c));
  }
}

KTEST(Ctype, converts_case_of_letters_only) {
  for (int c = EOF_VALUE; c <= 255; ++c) {
    KREQUIRE_EQ(klib::tolower(c), upper(c) ? c + ('a' - 'A') : c);
    KREQUIRE_EQ(klib::toupper(c), lower(c) ? c - ('a' - 'A') : c);
  }
}

KTEST(Ctype, works_in_constant_expressions) {
  static_assert(klib::isalpha('q') && !klib::isalpha('7') && klib::toupper('q') == 'Q');
  KEXPECT(true);
}
}  // namespace ktest
