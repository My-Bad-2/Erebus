#ifndef EREBUS_KLIBC_INCLUDE_STRING_H
#define EREBUS_KLIBC_INCLUDE_STRING_H

#include <stddef.h>
#include "sys/compiler.h"

#ifdef __cplusplus
namespace klibc {
	extern "C" {
#endif

	[[gnu::nonnull(1), gnu::returns_nonnull, gnu::flatten]]
	void *memset(void *dest, int ch, size_t count) noexcept;

	[[gnu::nonnull(1, 2), gnu::returns_nonnull, gnu::flatten]]
	void *memcpy(void *__restrict dest, const void *__restrict src, size_t count) noexcept;

	[[gnu::nonnull(1, 2), gnu::returns_nonnull, gnu::flatten]]
	void *memmove(void *dest, const void *src, size_t count) noexcept;

	[[gnu::nonnull(1, 2), gnu::flatten]]
	int memcmp(const void *s1, const void *s2, size_t count) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	void *memchr(const void *ptr, int ch, size_t count) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	size_t strlen(const char *str) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	size_t strnlen(const char *str, size_t maxlen) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	int strncmp(const char *s1, const char *s2, size_t count) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	int strcmp(const char *s1, const char *s2) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	char *strcpy(char *__restrict dest, const char *__restrict src) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	char *strncpy(char *__restrict dest, const char *__restrict src, size_t count) noexcept;

	[[gnu::nonnull(1, 2), gnu::returns_nonnull, gnu::flatten]]
	char *strcat(char *__restrict dest, const char *__restrict src) noexcept;

	[[gnu::nonnull(1, 2), gnu::returns_nonnull, gnu::flatten]]
	char *strncat(char *__restrict dest, const char *__restrict src, size_t count) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	char *strchr(const char *str, int ch) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	char *strrchr(const char *str, int ch) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	size_t strspn(const char *dest, const char *src) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	size_t strcspn(const char *dest, const char *src) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	char *strpbrk(const char *dest, const char *src) noexcept;

	[[gnu::nonnull(1, 2), gnu::pure, gnu::flatten]]
	char *strstr(const char *dest, const char *src) noexcept;

	[[gnu::nonnull(2, 3), gnu::flatten]]
	char *strtok_r(char *__restrict str, const char *__restrict delim, char **__restrict saveptr) noexcept;

	[[gnu::nonnull(2), gnu::flatten]]
	char *strtok(char *__restrict str, const char *__restrict delim) noexcept;

#ifdef __cplusplus
	}
}
#endif

#endif // EREBUS_KLIBC_INCLUDE_STRING_H
