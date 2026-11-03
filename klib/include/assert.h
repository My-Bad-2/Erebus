/// @file assert.h
/// @brief ISO C assert(), reporting through the glibc-compatible `__assert_fail` hook.
///
/// The kernel defines `__assert_fail` to halt with the failed condition and where it is. Defining
/// NDEBUG, as release builds do, compiles the checks out.

#include "internal/compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

[[noreturn]] void __assert_fail(const char* assertion, const char* file, unsigned int line,
                                const char* function) noexcept;

#ifdef __cplusplus
}
#endif

#undef assert

#ifdef NDEBUG
#define assert(...) ((void)0)
#else
#define assert(...) \
  ((__VA_ARGS__) ? (void)0 : __assert_fail(#__VA_ARGS__, __FILE__, __LINE__, __func__))
#endif
