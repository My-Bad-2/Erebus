#include "../include/string.hpp"
#include "string.h"

namespace klib {
namespace {
// Pointer to the byte that bit 7 of byte `bit / 8` of the word at `word` flags.
[[gnu::always_inline]] inline char* byte_at(const std::uintptr_t word, const int bit) noexcept {
  return reinterpret_cast<char*>(word + static_cast<unsigned>(bit) / 8);
}
}  // namespace

// Both functions read aligned words only, so they never touch a page past the terminator.

char* strchr(const char* str, const int ch) noexcept {
  using string::has_zero_byte;

  const std::uint64_t pattern = string::broadcast_byte(static_cast<std::uint8_t>(ch));
  auto word = reinterpret_cast<std::uintptr_t>(str) & ~std::uintptr_t{7};

  // The bytes before `str` in its first word are not part of it: make them match neither the
  // character nor the terminator, so that no borrow out of them fakes a match.
  const std::uint64_t garbage = string::low_bytes(string::word_offset(str));
  std::uint64_t w = string::load_aligned_word(str);
  std::uint64_t hit = has_zero_byte((w ^ pattern) | garbage);
  std::uint64_t end = has_zero_byte(w | garbage);

  while ((hit | end) == 0) {
    word += 8;
    w = string::unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(word));
    hit = has_zero_byte(w ^ pattern);
    end = has_zero_byte(w);
  }

  // The lowest flagged byte of each mask is exact. When `ch` is '\0' both point at the
  // terminator, which is what ISO C asks for.
  const int hit_bit = std::countr_zero(hit);
  return hit_bit <= std::countr_zero(end) ? byte_at(word, hit_bit) : nullptr;
}

char* strrchr(const char* str, const int ch) noexcept {
  using string::zero_bytes;

  const std::uint64_t pattern = string::broadcast_byte(static_cast<std::uint8_t>(ch));
  auto word = reinterpret_cast<std::uintptr_t>(str) & ~std::uintptr_t{7};

  // The last match wins, so the masks must be exact, not just exact at their lowest bit.
  const std::uint64_t valid = ~string::low_bytes(string::word_offset(str));
  std::uint64_t w = string::load_aligned_word(str);
  std::uint64_t hit = zero_bytes(w ^ pattern) & valid;
  std::uint64_t end = zero_bytes(w) & valid;

  char* last = nullptr;
  while (end == 0) {
    if (hit != 0) {
      last = byte_at(word, 63 - std::countl_zero(hit));
    }

    word += 8;
    w = string::unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(word));
    hit = zero_bytes(w ^ pattern);
    end = zero_bytes(w);
  }

  // Keep the matches up to and including the terminator; including it makes '\0' find itself.
  const std::uint64_t lowest_end = end & -end;
  hit &= lowest_end | (lowest_end - 1);
  return hit != 0 ? byte_at(word, 63 - std::countl_zero(hit)) : last;
}
}  // namespace klib
