#ifndef EREBUS_KLIBC_SRC_INCLUDE_SYS_COMPILER_HPP
#define EREBUS_KLIBC_SRC_INCLUDE_SYS_COMPILER_HPP

#ifndef __cplusplus
#define noexcept
#endif

#ifndef __cplusplus
#define constexpr_func static inline
#else
#define constexpr_func constexpr
#endif

#endif // EREBUS_KLIBC_SRC_INCLUDE_SYS_COMPILER_HPP
