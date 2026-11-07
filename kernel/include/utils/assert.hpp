/// @file assert.hpp
/// @brief Debug-build assertions.

#pragma once

#include <source_location>

namespace kernel::utils {
/// @brief Reports a failed KASSERT and halts the machine.
[[noreturn, gnu::cold, gnu::noinline]] void assertion_failed(
    const char* condition, std::source_location where = std::source_location::current()) noexcept;
}  // namespace kernel::utils

#ifdef NDEBUG
#define KASSERT(cond) static_cast<void>(0)
#else
#define KASSERT(cond)                                                  \
  (__builtin_expect(static_cast<bool>(cond), 1) ? static_cast<void>(0) \
                                                : ::kernel::utils::assertion_failed(#cond))
#endif
