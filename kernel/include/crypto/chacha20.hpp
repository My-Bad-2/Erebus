/// @file chacha20.hpp
/// @brief The ChaCha20 stream cipher (RFC 8439), and the random generator built on it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kernel::crypto {
namespace chacha20 {
inline constexpr std::size_t KEY_BYTES = 32;
inline constexpr std::size_t NONCE_BYTES = 12;
inline constexpr std::size_t BLOCK_BYTES = 64;

using Key = std::array<std::byte, KEY_BYTES>;
using Nonce = std::array<std::byte, NONCE_BYTES>;

/// @brief The RFC 8439 block function: 64 bytes of keystream for block `counter`.
void block(const Key& key, std::uint32_t counter, const Nonce& nonce,
           std::span<std::byte, BLOCK_BYTES> out) noexcept;

/// @brief Writes keystream into `out`, starting at block `counter`.
///
/// The block counter is 32 bits wide, so one key and nonce give 256 GiB of keystream; asking
/// for more wraps around and repeats it.
void keystream(const Key& key, std::uint32_t counter, const Nonce& nonce,
               std::span<std::byte> out) noexcept;

/// @brief XORs keystream into `data`, starting at block `counter`: encrypts or decrypts.
void xor_stream(const Key& key, std::uint32_t counter, const Nonce& nonce,
                std::span<std::byte> data) noexcept;
}  // namespace chacha20

/// @brief Bernstein's fast-key-erasure random generator over ChaCha20.
///
/// Each refill turns the key into one block of keystream. The first half of the block becomes
/// the next key, the second half is output, and every byte is wiped as soon as it has been
/// handed out. Someone who later reads the generator's memory therefore learns nothing about
/// what it produced before. Larger requests get the rest of that key's stream directly.
///
/// The generator only stretches its seed; reseed() it with fresh entropy from time to time. It
/// is not thread-safe, and has no destructor so that it can live in per-CPU data: call wipe()
/// when a copy is no longer needed.
class ChaChaRng {
  chacha20::Key m_key{};
  // Unused output, at the end of the buffer.
  std::array<std::byte, chacha20::BLOCK_BYTES - chacha20::KEY_BYTES> m_buffer{};
  std::uint32_t m_available = 0;

  void take_buffered(std::span<std::byte>& out) noexcept;

 public:
  constexpr ChaChaRng() noexcept = default;

  /// @brief Replaces the key with `seed` and forgets any buffered output.
  void reseed(const chacha20::Key& seed) noexcept;

  /// @brief Fills `out` with output.
  void fill(std::span<std::byte> out) noexcept;

  /// @brief Wipes the key and buffered output. The generator must be reseeded before use.
  void wipe() noexcept;
};
}  // namespace kernel::crypto
