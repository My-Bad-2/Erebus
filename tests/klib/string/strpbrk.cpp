#include <string.h>

#include "ktest.hpp"

namespace ktest::strings {
KTEST(Strpbrk, finds_the_first_character_of_a_set) {
  const char* str = "ErebusOSInit";
  KEXPECT_EQ(klib::strpbrk(str, "zxO"), str + 6);
  KEXPECT_EQ(klib::strpbrk(str, "I"), str + 8);
  KEXPECT_EQ(klib::strpbrk(str, "Z"), nullptr);
  KEXPECT_EQ(klib::strpbrk(str, ""), nullptr);
}

KTEST(Strstr, finds_the_first_occurrence) {
  const char* str = "Erebus OS Kernel";
  KEXPECT_EQ(klib::strstr(str, "OS"), str + 7);
  KEXPECT_EQ(klib::strstr(str, "Kernel"), str + 10);
  KEXPECT_EQ(klib::strstr(str, "Erebus"), str);
  KEXPECT_EQ(klib::strstr(str, ""), str);
  KEXPECT_EQ(klib::strstr("Short", "ShortString"), nullptr);
  KEXPECT_EQ(klib::strstr("a needle at the very end, StringX", "StringXY"), nullptr);
}

// Repeated prefixes are where a naive search goes wrong or slow.
KTEST(Strstr, handles_repeating_prefixes) {
  const char* str = "mississippi AAAAAAAAAAAAAAAAB";
  KEXPECT_EQ(klib::strstr(str, "issip"), str + 4);
  KEXPECT_EQ(klib::strstr(str, "issi"), str + 1);
  KEXPECT_EQ(klib::strstr(str, "AAAAAB"), str + 23);
  KEXPECT_EQ(klib::strstr(str, "AAAAAC"), nullptr);
}
}  // namespace ktest::strings
