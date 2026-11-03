/// @file stdlib.h
/// @brief The parts of <stdlib.h> that make sense without a hosted runtime: number parsing,
///        sorting and searching, and integer arithmetic.
///
/// There is no errno: the conversion functions report overflow only through their clamped
/// result, and through `endptr`.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "internal/compiler.h"

KLIB_BEGIN_DECLS

typedef struct {
  int quot;
  int rem;
} div_t;

typedef struct {
  long quot;
  long rem;
} ldiv_t;

typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

typedef struct {
  intmax_t quot;
  intmax_t rem;
} imaxdiv_t;

/// @brief Parses a decimal integer after optional whitespace and sign. Stops at the first
///        non-digit; out-of-range values wrap rather than being undefined.
[[gnu::nonnull(1), gnu::pure]]
int atoi(const char* str) noexcept;

/// @copydoc atoi
[[gnu::nonnull(1), gnu::pure]]
long atol(const char* str) noexcept;

/// @copydoc atoi
[[gnu::nonnull(1), gnu::pure]]
long long atoll(const char* str) noexcept;

/// @brief Parses an integer in `base` (2 to 36, or 0 to tell from a `0x` or `0` prefix).
///
/// Values out of range clamp to LONG_MIN or LONG_MAX. If `endptr` is not null it receives the
/// first character not parsed, or `str` itself when there were no digits or `base` is invalid.
[[gnu::nonnull(1)]]
long strtol(const char* __restrict str, char** __restrict endptr, int base) noexcept;

/// @copydoc strtol
[[gnu::nonnull(1)]]
long long strtoll(const char* __restrict str, char** __restrict endptr, int base) noexcept;

/// @brief strtol() for unsigned results: out-of-range values clamp to ULONG_MAX, and a leading
///        '-' negates the result modulo 2^64, as ISO C specifies.
[[gnu::nonnull(1)]]
unsigned long strtoul(const char* __restrict str, char** __restrict endptr, int base) noexcept;

/// @copydoc strtoul
[[gnu::nonnull(1)]]
unsigned long long strtoull(const char* __restrict str, char** __restrict endptr,
                            int base) noexcept;

/// @brief Sorts `count` elements of `size` bytes in O(n log n) worst case (introsort). Not
///        stable.
[[gnu::nonnull(4)]]
void qsort(void* ptr, size_t count, size_t size, int (*comp)(const void*, const void*)) noexcept;

/// @brief Finds an element equal to `key` in an array sorted by `comp`, or null. Gallops from
///        the front first, so keys near the start are found in fewer comparisons.
[[gnu::nonnull(1, 5)]]
void* bsearch(const void* key, const void* ptr, size_t count, size_t size,
              int (*comp)(const void*, const void*)) noexcept;

/// @brief Absolute value. The most negative value stays as it is.
[[gnu::const]] int abs(int j) noexcept;
/// @copydoc abs
[[gnu::const]] long labs(long j) noexcept;
/// @copydoc abs
[[gnu::const]] long long llabs(long long j) noexcept;
/// @copydoc abs
[[gnu::const]] intmax_t imaxabs(intmax_t j) noexcept;

/// @brief Quotient and remainder of one division, rounding towards zero.
[[gnu::const]] div_t div(int numer, int denom) noexcept;
/// @copydoc div
[[gnu::const]] ldiv_t ldiv(long numer, long denom) noexcept;
/// @copydoc div
[[gnu::const]] lldiv_t lldiv(long long numer, long long denom) noexcept;
/// @copydoc div
[[gnu::const]] imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) noexcept;

KLIB_END_DECLS
