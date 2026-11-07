/// @file string.hpp
/// @brief Word-at-a-time helpers shared by the klib memory and string functions.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace klib::string {
template <typename T>
struct [[gnu::packed, gnu::may_alias]] Unaligned {
  T val;
};

/// @brief Stores an integer at any address, without the aliasing rules getting in the way.
template <typename T>
  requires std::is_integral_v<T>
[[gnu::always_inline]] void unaligned_store(void* ptr, const T val) noexcept {
  static_cast<Unaligned<T>*>(ptr)->val = val;
}

/// @brief Loads an integer from any address, without the aliasing rules getting in the way.
template <typename T>
  requires std::is_integral_v<T>
[[nodiscard, gnu::always_inline]] T unaligned_load(const void* ptr) noexcept {
  return static_cast<const Unaligned<T>*>(ptr)->val;
}

/// @brief Sets bit 7 of every zero byte of `v`, and of some bytes above the first zero byte.
///
/// Cheap, and enough wherever only the lowest set bit matters: no byte below the first zero byte
/// is ever flagged, so std::countr_zero() finds the first zero exactly. A borrow out of a zero
/// byte can flag a 0x01 byte right above it.
template <std::unsigned_integral T>
[[nodiscard, gnu::const, gnu::always_inline]] constexpr T has_zero_byte(const T v) noexcept {
  constexpr T ones = static_cast<T>(~T{0}) / 0xFF;  // 0x0101...
  constexpr T highs = static_cast<T>(ones << 7);    // 0x8080...
  return static_cast<T>((v - ones) & ~v & highs);
}

/// @brief Sets bit 7 of exactly the zero bytes of `v`. For when the highest match matters.
[[nodiscard, gnu::const, gnu::always_inline]] constexpr std::uint64_t zero_bytes(
    const std::uint64_t v) noexcept {
  constexpr std::uint64_t lows = 0x7F7F'7F7F'7F7F'7F7Ful;
  // Adding 0x7F to the low seven bits of a byte carries into bit 7 unless they are all zero, and
  // never into the next byte.
  return ~(((v & lows) + lows) | v | lows);
}

/// @brief Copies `c` into every byte of a word.
[[nodiscard, gnu::const, gnu::always_inline]] constexpr std::uint64_t broadcast_byte(
    const std::uint8_t c) noexcept {
  return std::uint64_t{c} * 0x0101'0101'0101'0101ul;
}

/// @brief Mask of the `n` lowest bytes of a word, for `n` from 0 to 7.
[[nodiscard, gnu::const, gnu::always_inline]] constexpr std::uint64_t low_bytes(
    const std::uint32_t n) noexcept {
  return (std::uint64_t{1} << (n * 8)) - 1;
}

/// @brief Offset of `ptr` within its aligned 8-byte word.
[[nodiscard, gnu::always_inline]] inline std::uint32_t word_offset(const void* ptr) noexcept {
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(ptr) & 7);
}

/// @brief The aligned 8-byte word holding `ptr`. Never crosses into another page, even when
///        `ptr` is the last byte of one.
[[nodiscard, gnu::always_inline]] inline std::uint64_t load_aligned_word(const void* ptr) noexcept {
  const auto addr = reinterpret_cast<std::uintptr_t>(ptr) & ~std::uintptr_t{7};
  return unaligned_load<std::uint64_t>(reinterpret_cast<const void*>(addr));
}

[[gnu::always_inline]] inline void rep_movsb(void* dest, const void* src,
                                             std::size_t count) noexcept {
  asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(count) : : "memory");
}

[[gnu::always_inline]] inline void rep_stosb(void* dest, const std::uint8_t val,
                                             std::size_t count) noexcept {
  asm volatile("rep stosb" : "+D"(dest), "+c"(count) : "a"(val) : "memory");
}

/// @brief Copies one `T` from `src + offset` to `dest + offset`.
template <typename T>
[[gnu::always_inline]] void copy_chunk(const std::uintptr_t dest, const std::uintptr_t src,
                                       const std::size_t offset) noexcept {
  const auto val = unaligned_load<T>(reinterpret_cast<const void*>(src + offset));
  unaligned_store<T>(reinterpret_cast<void*>(dest + offset), val);
}

/// @brief Copies one `T` per offset, loading all of them before storing any, so that the copy is
///        right however the two ranges overlap.
template <typename T, typename... Offsets>
[[gnu::always_inline]] void transfer(const std::uintptr_t dest, const std::uintptr_t src,
                                     const Offsets... offsets) noexcept {
  const T snapshot[] = {
      unaligned_load<T>(reinterpret_cast<const void*>(src + static_cast<std::size_t>(offsets)))...};

  std::size_t index = 0;
  (unaligned_store<T>(reinterpret_cast<void*>(dest + static_cast<std::size_t>(offsets)),
                      snapshot[index++]),
   ...);
}

/// @brief memcmp() of two differing little-endian words: the difference of their first
///        differing bytes in memory order.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] int diff_bytes(const T v1, const T v2) noexcept {
  const auto shift = static_cast<std::uint32_t>(std::countr_zero(static_cast<T>(v1 ^ v2))) & ~7u;
  return static_cast<int>((v1 >> shift) & 0xFF) - static_cast<int>((v2 >> shift) & 0xFF);
}

/// @brief Compares one `T` at `offset` in both buffers: 0 if equal, else memcmp()'s answer.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] int check_chunk(const std::uintptr_t p1, const std::uintptr_t p2,
                                                  const std::size_t offset) noexcept {
  const auto v1 = unaligned_load<T>(reinterpret_cast<const void*>(p1 + offset));
  const auto v2 = unaligned_load<T>(reinterpret_cast<const void*>(p2 + offset));
  return v1 == v2 ? 0 : diff_bytes(v1, v2);
}

/// @brief The first byte equal to the low byte of `pattern` within one `T` at `base + offset`,
///        or null.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] void* find_in_chunk(const std::uintptr_t base,
                                                      const std::size_t offset,
                                                      const std::uint64_t pattern) noexcept {
  const auto v = unaligned_load<T>(reinterpret_cast<const void*>(base + offset));
  if (const T mask = has_zero_byte<T>(static_cast<T>(v ^ static_cast<T>(pattern)))) {
    return reinterpret_cast<void*>(base + offset + (std::countr_zero(mask) / 8));
  }

  return nullptr;
}
}  // namespace klib::string
