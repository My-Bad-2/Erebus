/// @file ctype.h
/// @brief Character classification for the "C" locale, i.e. plain ASCII.
///
/// Like ISO C, the functions take an `int` holding an unsigned char value or EOF. Each is a
/// single range check: `(c & 0xFF) - lo + 0u < n` wraps below `lo` to a huge unsigned value, so
/// one comparison covers both ends of the range. Bytes 0x80 to 0xFF and EOF belong to no class.
#pragma once

#include "internal/compiler.h"

KLIB_BEGIN_DECLS

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool islower(const int c) noexcept {
  return (c & 0xFF) - 'a' + 0u < 26u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isupper(const int c) noexcept {
  return (c & 0xFF) - 'A' + 0u < 26u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isalpha(const int c) noexcept {
  // Setting bit 5 folds upper case onto lower case.
  return ((c & 0xFF) | 0x20) - 'a' + 0u < 26u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isdigit(const int c) noexcept {
  return (c & 0xFF) - '0' + 0u < 10u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isalnum(const int c) noexcept {
  return isalpha(c) || isdigit(c);
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isxdigit(const int c) noexcept {
  return isdigit(c) || ((c & 0xFF) | 0x20) - 'a' + 0u < 6u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isspace(const int c) noexcept {
  // ' ', and '\t' through '\r'.
  return (c & 0xFF) == ' ' || (c & 0xFF) - '\t' + 0u < 5u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isblank(const int c) noexcept {
  return (c & 0xFF) == ' ' || (c & 0xFF) == '\t';
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isprint(const int c) noexcept {
  // ' ' through '~'.
  return (c & 0xFF) - ' ' + 0u < 95u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool isgraph(const int c) noexcept {
  // '!' through '~'.
  return (c & 0xFF) - '!' + 0u < 94u;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool iscntrl(const int c) noexcept {
  return (c & 0xFF) < 0x20 || (c & 0xFF) == 0x7F;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE bool ispunct(const int c) noexcept {
  return isgraph(c) && !isalnum(c);
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE int tolower(const int c) noexcept {
  return isupper(c) ? (c | 0x20) : c;
}

[[gnu::const, gnu::always_inline]] KLIB_INLINE int toupper(const int c) noexcept {
  return islower(c) ? (c & ~0x20) : c;
}

KLIB_END_DECLS
