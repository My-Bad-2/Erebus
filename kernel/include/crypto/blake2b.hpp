/// @file blake2b.hpp
/// @brief BLAKE2b (RFC 7693): a cryptographic hash, and with a key a MAC or a PRF.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "crypto/bytes.hpp"

namespace kernel::crypto {
/// @brief Incremental BLAKE2b with a digest of 1 to 64 bytes and an optional key of up to 64.
///
/// @code
/// Blake2b hash{32};
/// hash.update(part1);
/// hash.update(part2);
/// hash.finalize(digest);  // 32 bytes
/// @endcode
///
/// The state is wiped when the object is destroyed. It may be copied, to finalize a snapshot
/// while the original keeps absorbing input.
class Blake2b {
 public:
  static constexpr std::size_t BLOCK_BYTES = 128;
  static constexpr std::size_t MAX_DIGEST_BYTES = 64;
  static constexpr std::size_t MAX_KEY_BYTES = 64;

  /// @brief Starts a hash with a `digest_bytes`-byte result, keyed with `key` if it is not
  ///        empty. Out-of-range sizes are clamped into range.
  constexpr explicit Blake2b(std::size_t digest_bytes = MAX_DIGEST_BYTES,
                             std::span<const std::byte> key = {}) noexcept {
    digest_bytes = digest_bytes == 0 ? 1 : digest_bytes;
    digest_bytes = digest_bytes > MAX_DIGEST_BYTES ? MAX_DIGEST_BYTES : digest_bytes;
    const std::size_t key_bytes = key.size() > MAX_KEY_BYTES ? MAX_KEY_BYTES : key.size();

    m_h = IV;
    // The parameter block, reduced to what sequential hashing uses: digest length, key length,
    // fanout 1 and depth 1.
    m_h[0] ^= 0x0101'0000 ^ (std::uint64_t{key_bytes} << 8) ^ digest_bytes;
    m_digest_bytes = static_cast<std::uint32_t>(digest_bytes);

    // A key is hashed as a first block of its own, padded with zeros. It stays buffered like any
    // other input, so that it gets the final flag if nothing follows.
    if (key_bytes != 0) {
      for (std::size_t i = 0; i < key_bytes; ++i) {
        m_buf[i] = key[i];
      }

      m_buf_len = BLOCK_BYTES;
    }
  }

  constexpr Blake2b(const Blake2b&) noexcept = default;
  constexpr Blake2b& operator=(const Blake2b&) noexcept = default;

  ~Blake2b() noexcept { wipe(); }

  /// @brief Absorbs `data`.
  void update(std::span<const std::byte> data) noexcept;

  /// @brief Writes the digest to `out`: the first `out.size()` bytes of it, at most
  ///        digest_bytes(). Leaves the object wiped; start a new one to hash again.
  void finalize(std::span<std::byte> out) noexcept;

  [[nodiscard]] constexpr std::size_t digest_bytes() const noexcept { return m_digest_bytes; }

  /// @brief One-shot hash of `in` into `out`, whose size is the digest length.
  static void hash(std::span<std::byte> out, std::span<const std::byte> in,
                   std::span<const std::byte> key = {}) noexcept;

 private:
  static constexpr std::array<std::uint64_t, 8> IV = {
      0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
      0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
  };

  void compress(bool last) noexcept;
  void wipe() noexcept;

  std::array<std::uint64_t, 8> m_h{};
  // Bytes hashed so far, as a 128-bit counter.
  std::uint64_t m_t0 = 0;
  std::uint64_t m_t1 = 0;
  std::array<std::byte, BLOCK_BYTES> m_buf{};
  std::uint32_t m_buf_len = 0;
  std::uint32_t m_digest_bytes = MAX_DIGEST_BYTES;
};
}  // namespace kernel::crypto
