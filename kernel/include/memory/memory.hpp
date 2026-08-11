#pragma once

#include <cstddef>

namespace kernel::memory {
inline constexpr std::size_t PAGE_SIZE = 4096;
inline constexpr std::size_t PAGE_SIZE_2MB = PAGE_SIZE * 512;
inline constexpr std::size_t PAGE_SIZE_1GB = PAGE_SIZE_2MB * 512;

void initialize();
} // namespace kernel::memory