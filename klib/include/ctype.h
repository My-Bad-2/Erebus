#pragma once

#include "internal/compiler.h"
#include <stdint.h>

#ifdef __cplusplus
namespace klib {
#ifndef CTYPE_TEST
extern "C" {
#endif
#endif

[[gnu::const, gnu::always_inline]] constexpr_func bool islower(const int c) noexcept {
  return static_cast<unsigned>(static_cast<uint8_t>(c) - 'a') < 26;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isupper(const int c) noexcept {
  return static_cast<unsigned>(static_cast<uint8_t>(c) - 'A') < 26;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isalpha(const int c) noexcept {
  // Bitwise OR with 0x20 forces uppercase ASCII letters to lowercase, allowing us to check both cases in a single
  // math operation.
  return ((static_cast<uint8_t>(c) | 0x20U) - 'a') < 26;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isdigit(const int c) noexcept {
  return static_cast<unsigned>(static_cast<uint8_t>(c) - '0') < 10;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isalnum(const int c) noexcept {
  return isalpha(c) || isdigit(c);
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isxdigit(const int c) noexcept {
  return isdigit(c) || ((static_cast<uint8_t>(c) | 0x20U) - 'a') < 6;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isspace(const int c) noexcept {
  // '\t' is 9, '\n' is 10, '\v' is 11, '\f' is 12, '\r' is 13.
  return c == ' ' || static_cast<unsigned>(static_cast<uint8_t>(c) - '\t') < 5;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isblank(const int c) noexcept { return c == ' ' || c == '\t'; }

[[gnu::const, gnu::always_inline]] constexpr_func bool isprint(const int c) noexcept {
  // ' ' (32) to '~' (126). Range length is 94 (0x5F).
  return static_cast<unsigned>(static_cast<uint8_t>(c) - 0x20) < 0x5F;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool isgraph(const int c) noexcept {
  // '!' (33) to '~' (126). Range length is 93 (0x5E).
  return static_cast<unsigned>(static_cast<uint8_t>(c) - 0x21) < 0x5E;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool iscntrl(const int c) noexcept {
  return static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) == 0x7F;
}

[[gnu::const, gnu::always_inline]] constexpr_func bool ispunct(const int c) noexcept {
  return isgraph(c) && !isalnum(c);
}

[[gnu::const, gnu::always_inline]] constexpr_func int tolower(const int c) noexcept {
  // If upper, add 0x20 to shift to lower.
  return isupper(c) ? (c | 0x20) : c;
}

[[gnu::const, gnu::always_inline]] constexpr_func int toupper(const int c) noexcept {
  // If lower, clear the 0x20 bit to shift to upper.
  return islower(c) ? (c & ~0x20) : c;
}

#ifdef __cplusplus
#ifndef CTYPE_TEST
}
#endif
} // namespace klib
#endif
