/// @file string.h
/// @brief The kernel's <string.h>: memory and string functions, tuned for x86-64.
///
/// The memory functions handle small sizes with a few overlapping unaligned loads and stores and
/// hand large ones to `rep movsb` / `rep stosb`. The string functions read a word at a time, but
/// never from a page the string does not reach, so they cannot fault past its end.
#pragma once

#include <stddef.h>

#include "internal/compiler.h"

KLIB_BEGIN_DECLS

/// @brief Fills `count` bytes at `dest` with `(unsigned char)ch`. Returns `dest`.
void* memset(void* dest, int ch, size_t count) noexcept;

/// @brief Copies `count` bytes from `src` to `dest`, which must not overlap. Returns `dest`.
void* memcpy(void* __restrict dest, const void* __restrict src, size_t count) noexcept;

/// @brief Copies `count` bytes from `src` to `dest`, which may overlap. Returns `dest`.
void* memmove(void* dest, const void* src, size_t count) noexcept;

/// @brief Compares `count` bytes as unsigned char: negative, zero or positive as `s1` sorts
///        before, with or after `s2`.
[[gnu::pure]]
int memcmp(const void* s1, const void* s2, size_t count) noexcept;

/// @brief The first of the `count` bytes at `ptr` equal to `(unsigned char)ch`, or null.
[[gnu::pure]]
void* memchr(const void* ptr, int ch, size_t count) noexcept;

/// @brief Length of `str`, not counting the terminator.
[[gnu::nonnull(1), gnu::pure]]
size_t strlen(const char* str) noexcept;

/// @brief Length of `str`, but at most `maxlen`.
[[gnu::nonnull(1), gnu::pure]]
size_t strnlen(const char* str, size_t maxlen) noexcept;

/// @brief Compares at most `count` characters, as unsigned char.
[[gnu::nonnull(1, 2), gnu::pure]]
int strncmp(const char* s1, const char* s2, size_t count) noexcept;

/// @brief Compares two strings, as unsigned char.
[[gnu::nonnull(1, 2), gnu::pure]]
int strcmp(const char* s1, const char* s2) noexcept;

/// @brief Copies `src`, terminator included, to `dest`. Returns `dest`.
[[gnu::nonnull(1, 2), gnu::returns_nonnull]]
char* strcpy(char* __restrict dest, const char* __restrict src) noexcept;

/// @brief Copies at most `count` characters of `src` to `dest` and pads the rest of the `count`
///        bytes with zeros. Like ISO C, leaves `dest` unterminated if `src` is `count` long or
///        longer. Returns `dest`.
[[gnu::nonnull(1, 2), gnu::returns_nonnull]]
char* strncpy(char* __restrict dest, const char* __restrict src, size_t count) noexcept;

/// @brief Appends `src` to `dest`. Returns `dest`.
[[gnu::nonnull(1, 2), gnu::returns_nonnull]]
char* strcat(char* __restrict dest, const char* __restrict src) noexcept;

/// @brief Appends at most `count` characters of `src` to `dest`, then a terminator. Returns
///        `dest`.
[[gnu::nonnull(1, 2), gnu::returns_nonnull]]
char* strncat(char* __restrict dest, const char* __restrict src, size_t count) noexcept;

/// @brief The first `(char)ch` in `str`, or null. Searching for '\0' finds the terminator.
[[gnu::nonnull(1), gnu::pure]]
char* strchr(const char* str, int ch) noexcept;

/// @brief The last `(char)ch` in `str`, or null. Searching for '\0' finds the terminator.
[[gnu::nonnull(1), gnu::pure]]
char* strrchr(const char* str, int ch) noexcept;

/// @brief Length of the leading run of `str` made only of characters in `accept`.
[[gnu::nonnull(1, 2), gnu::pure]]
size_t strspn(const char* str, const char* accept) noexcept;

/// @brief Length of the leading run of `str` made only of characters not in `reject`.
[[gnu::nonnull(1, 2), gnu::pure]]
size_t strcspn(const char* str, const char* reject) noexcept;

/// @brief The first character of `str` that is in `accept`, or null.
[[gnu::nonnull(1, 2), gnu::pure]]
char* strpbrk(const char* str, const char* accept) noexcept;

/// @brief The first occurrence of `needle` in `haystack`, or null. An empty needle matches at
///        the start.
[[gnu::nonnull(1, 2), gnu::pure]]
char* strstr(const char* haystack, const char* needle) noexcept;

/// @brief Splits `str` into tokens separated by characters of `delim`, keeping its place in
///        `*saveptr`. Pass `str` on the first call and null on the following ones.
[[gnu::nonnull(2, 3)]]
char* strtok_r(char* __restrict str, const char* __restrict delim,
               char** __restrict saveptr) noexcept;

/// @brief strtok_r() with a hidden, shared `saveptr`. Not safe to use from two places at once.
[[gnu::nonnull(2)]]
char* strtok(char* __restrict str, const char* __restrict delim) noexcept;

KLIB_END_DECLS
