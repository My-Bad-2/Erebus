/// @file compiler.h
/// @brief Macros that let the klib C headers compile as C and as C++.
#pragma once

#ifdef __cplusplus

#define KLIB_BEGIN_DECLS \
  namespace klib {       \
  extern "C" {
#define KLIB_END_DECLS \
  }                    \
  }

/// @brief Marks a header-defined function constexpr in C++, static inline in C.
#define KLIB_INLINE constexpr

#else

#define KLIB_BEGIN_DECLS
#define KLIB_END_DECLS
#define KLIB_INLINE static inline
#define noexcept

#endif
