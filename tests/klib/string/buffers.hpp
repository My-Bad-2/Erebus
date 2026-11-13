// Buffers and reference loops shared by the klib string tests.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ktest::strings {
inline constexpr std::size_t BUFFER_BYTES = 4096;
inline constexpr std::uint8_t SENTINEL = 0xAA;

// Every size class the word-at-a-time code distinguishes, and some beyond.
inline constexpr std::size_t SIZES[] = {0,  1,  2,  3,  4,  5,  7,  8,   9,   15,  16,
                                        17, 31, 32, 33, 63, 64, 65, 127, 128, 512, 1024};

// Fills `bytes` with a pattern that differs from byte to byte and from seed to seed.
inline void fill_pattern(std::uint8_t* bytes, const std::size_t count,
                         const std::uint8_t seed) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<std::uint8_t>(i * 7 + seed);
  }
}

inline void fill(std::uint8_t* bytes, const std::size_t count, const std::uint8_t value) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    bytes[i] = value;
  }
}

// Whether the `count` bytes at `a` and `b` are equal, compared one at a time.
[[nodiscard]] inline bool same(const std::uint8_t* a, const std::uint8_t* b,
                               const std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }

  return true;
}
}  // namespace ktest::strings
