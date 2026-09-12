#pragma once

#include <cstddef>

namespace kernel::memory {
inline constexpr std::size_t PAGE_SHIFT_4KB = 12;
inline constexpr std::size_t PAGE_SHIFT_2MB = 21;
inline constexpr std::size_t PAGE_SHIFT_1GB = 30;

inline constexpr std::size_t PAGE_SIZE = 1ul << PAGE_SHIFT_4KB;
inline constexpr std::size_t PAGE_SIZE_2MB = 1ul << PAGE_SHIFT_2MB;
inline constexpr std::size_t PAGE_SIZE_1GB = 1ul << PAGE_SHIFT_1GB;

void initialize();
} // namespace kernel::memory