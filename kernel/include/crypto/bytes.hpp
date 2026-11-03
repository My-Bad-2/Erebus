/// @file bytes.hpp
/// @brief Byte-level helpers for the crypto code: little-endian loads and stores, wiping
///        secrets, and comparing them in constant time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace kernel::crypto {
[[nodiscard, gnu::always_inline]] constexpr std::uint32_t load_le32(const std::byte* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

[[nodiscard, gnu::always_inline]] constexpr std::uint64_t load_le64(const std::byte* p) noexcept {
  const std::uint64_t low = load_le32(p);
  const std::uint64_t high = load_le32(p + 4);
  return low | high << 32;
}

[[gnu::always_inline]] constexpr void store_le32(std::byte* p, const std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<std::byte>(v >> (8 * i));
  }
}

[[gnu::always_inline]] constexpr void store_le64(std::byte* p, const std::uint64_t v) noexcept {
  store_le32(p, static_cast<std::uint32_t>(v));
  store_le32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

/// @brief Zeroes `bytes` in a way the compiler may not drop, even if nothing reads them again.
///
/// A plain memset() of a buffer that is about to go out of scope is a dead store, and gets
/// removed; the empty asm claims to read the memory, which keeps the store.
[[gnu::always_inline]] inline void secure_zero(const std::span<std::byte> bytes) noexcept {
  if (bytes.empty()) {
    return;
  }

  __builtin_memset(bytes.data(), 0, bytes.size());
  asm volatile("" : : "r"(bytes.data()) : "memory");
}

/// @brief secure_zero() of a whole object, which must be trivially copyable.
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[gnu::always_inline]] inline void secure_zero(T& object) noexcept {
  secure_zero(std::as_writable_bytes(std::span{&object, 1}));
}

/// @brief Whether `a` and `b` hold the same bytes, taking the same time wherever they differ.
///
/// For comparing secrets such as MACs, where an early exit would tell an attacker how many
/// leading bytes they got right. The lengths are not secret.
[[nodiscard]] inline bool equal_constant_time(const std::span<const std::byte> a,
                                              const std::span<const std::byte> b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }

  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  }

  // Keep the compiler from turning the loop back into an early exit.
  asm volatile("" : "+r"(diff));
  return diff == 0;
}
}  // namespace kernel::crypto
