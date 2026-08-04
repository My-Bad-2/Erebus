#ifndef EREBUS_klib_INCLUDE_STDLIB_H
#define EREBUS_klib_INCLUDE_STDLIB_H

#include <stddef.h>
#include <stdint.h>
#include "internal/compiler.h"

#ifdef __cplusplus
namespace klib {
	extern "C" {
#endif

	struct div_t {
		int quot;
		int rem;
	};

	struct ldiv_t {
		long quot;
		long rem;
	};

	struct lldiv_t {
		long long quot;
		long long rem;
	};

	struct imaxdiv_t {
		std::intmax_t quot;
		std::intmax_t rem;
	};

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	int atoi(const char *str) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	long int atol(const char *str) noexcept;

	[[gnu::nonnull(1), gnu::pure, gnu::flatten]]
	long long int atoll(const char *str) noexcept;

	[[gnu::nonnull(1), gnu::flatten]]
	long strtol(const char *__restrict str, char **__restrict endptr, int base) noexcept;

	[[gnu::nonnull(1), gnu::flatten]]
	long long strtoll(const char *__restrict str, char **__restrict endptr, int base) noexcept;

	[[gnu::nonnull(1), gnu::flatten]]
	unsigned long strtoul(const char *__restrict str, char **__restrict endptr, int base) noexcept;

	[[gnu::nonnull(1), gnu::flatten]]
	unsigned long long strtoull(const char *__restrict str, char **__restrict endptr, int base) noexcept;

	[[gnu::nonnull(1, 4), gnu::flatten]]
	void qsort(void *__restrict ptr, size_t count, size_t size, int (*comp)(const void *, const void *)) noexcept;

	[[gnu::nonnull(1, 2, 5), gnu::pure, gnu::flatten]]
	void *bsearch(const void *key, const void *ptr, std::size_t count, std::size_t size,
								int (*comp)(const void *, const void *)) noexcept;

	[[gnu::const, gnu::flatten]] int abs(int j) noexcept;
	[[gnu::const, gnu::flatten]] long labs(long j) noexcept;
	[[gnu::const, gnu::flatten]] long long llabs(long long j) noexcept;
	[[gnu::const, gnu::flatten]] intmax_t imaxabs(intmax_t j) noexcept;

	[[gnu::const, gnu::flatten]] div_t div(int numer, int denom) noexcept;
	[[gnu::const, gnu::flatten]] ldiv_t ldiv(long numer, long denom) noexcept;
	[[gnu::const, gnu::flatten]] lldiv_t lldiv(long long numer, long long denom) noexcept;
	[[gnu::const, gnu::flatten]] imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) noexcept;

#ifdef __cplusplus
	}
}
#endif

#endif // EREBUS_klib_INCLUDE_STDLIB_H
