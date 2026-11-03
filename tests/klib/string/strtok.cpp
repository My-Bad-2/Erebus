#include <string.h>

#include "ktest.hpp"

namespace kernel::testing::strings {
KTEST(StrtokR, splits_on_any_delimiter) {
  char str[] = "Erebus OS: Fast, Secure kernel";
  char* save = nullptr;
  KEXPECT_STREQ(klib::strtok_r(str, " :,", &save), "Erebus");
  KEXPECT_STREQ(klib::strtok_r(nullptr, " :,", &save), "OS");
  KEXPECT_STREQ(klib::strtok_r(nullptr, " :,", &save), "Fast");
  KEXPECT_STREQ(klib::strtok_r(nullptr, " :,", &save), "Secure");
  KEXPECT_STREQ(klib::strtok_r(nullptr, " :,", &save), "kernel");
  KEXPECT_EQ(klib::strtok_r(nullptr, " :,", &save), nullptr);
}

KTEST(StrtokR, skips_runs_of_delimiters) {
  char str[] = ",,,apple,,banana,cherry,,,";
  char* save = nullptr;
  KEXPECT_STREQ(klib::strtok_r(str, ",", &save), "apple");
  KEXPECT_STREQ(klib::strtok_r(nullptr, ",", &save), "banana");
  KEXPECT_STREQ(klib::strtok_r(nullptr, ",", &save), "cherry");
  KEXPECT_EQ(klib::strtok_r(nullptr, ",", &save), nullptr);

  char empty[] = "";
  KEXPECT_EQ(klib::strtok_r(empty, " ", &save), nullptr);
  char only_delimiters[] = " , ,";
  KEXPECT_EQ(klib::strtok_r(only_delimiters, " ,", &save), nullptr);
  char no_delimiters[] = "Kernel";
  KEXPECT_STREQ(klib::strtok_r(no_delimiters, " ,", &save), "Kernel");
  KEXPECT_EQ(klib::strtok_r(nullptr, " ,", &save), nullptr);
}

KTEST(StrtokR, keeps_interleaved_parses_apart) {
  char first[] = "A-B-C";
  char second[] = "1:2:3";
  char* save_first = nullptr;
  char* save_second = nullptr;
  KEXPECT_STREQ(klib::strtok_r(first, "-", &save_first), "A");
  KEXPECT_STREQ(klib::strtok_r(second, ":", &save_second), "1");
  KEXPECT_STREQ(klib::strtok_r(nullptr, "-", &save_first), "B");
  KEXPECT_STREQ(klib::strtok_r(nullptr, ":", &save_second), "2");
  KEXPECT_STREQ(klib::strtok_r(nullptr, "-", &save_first), "C");
  KEXPECT_STREQ(klib::strtok_r(nullptr, ":", &save_second), "3");
}

KTEST(Strtok, works_like_strtok_r) {
  char str[] = "Boot Init Kernel";
  KEXPECT_STREQ(klib::strtok(str, " "), "Boot");
  KEXPECT_STREQ(klib::strtok(nullptr, " "), "Init");
  KEXPECT_STREQ(klib::strtok(nullptr, " "), "Kernel");
  KEXPECT_EQ(klib::strtok(nullptr, " "), nullptr);
}
}  // namespace kernel::testing::strings
