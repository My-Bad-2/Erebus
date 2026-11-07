#include "crypto/blake2b.hpp"

#include <algorithm>
#include <bit>

namespace kernel::crypto {
namespace {
// Which message word each step of each round reads (RFC 7693, 2.7).
constexpr std::uint8_t SIGMA[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
};

[[gnu::always_inline]] void mix(std::uint64_t (&v)[16], int a, int b, int c, int d, std::uint64_t x,
                                std::uint64_t y) noexcept {
  v[a] = v[a] + v[b] + x;
  v[d] = std::rotr(v[d] ^ v[a], 32);
  v[c] = v[c] + v[d];
  v[b] = std::rotr(v[b] ^ v[c], 24);
  v[a] = v[a] + v[b] + y;
  v[d] = std::rotr(v[d] ^ v[a], 16);
  v[c] = v[c] + v[d];
  v[b] = std::rotr(v[b] ^ v[c], 63);
}
}  // namespace

void Blake2b::compress(const bool last) noexcept {
  std::uint64_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = load_le64(m_buf.data() + 8 * i);
  }

  std::uint64_t v[16];
  for (int i = 0; i < 8; ++i) {
    v[i] = m_h[i];
    v[i + 8] = IV[i];
  }

  v[12] ^= m_t0;
  v[13] ^= m_t1;
  if (last) {
    v[14] = ~v[14];
  }

  for (const auto& s : SIGMA) {
    mix(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
    mix(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
    mix(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
    mix(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
    mix(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
    mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    mix(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
    mix(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
  }

  for (int i = 0; i < 8; ++i) {
    m_h[i] ^= v[i] ^ v[i + 8];
  }

  secure_zero(m);
  secure_zero(v);
}

void Blake2b::update(std::span<const std::byte> data) noexcept {
  while (!data.empty()) {
    // A full buffer is only compressed once more input shows it was not the last block.
    if (m_buf_len == BLOCK_BYTES) {
      m_t0 += BLOCK_BYTES;
      m_t1 += m_t0 < BLOCK_BYTES ? 1 : 0;
      compress(false);
      m_buf_len = 0;
    }

    const std::size_t take = std::min(data.size(), BLOCK_BYTES - m_buf_len);
    for (std::size_t i = 0; i < take; ++i) {
      m_buf[m_buf_len + i] = data[i];
    }

    m_buf_len += static_cast<std::uint32_t>(take);
    data = data.subspan(take);
  }
}

void Blake2b::finalize(const std::span<std::byte> out) noexcept {
  m_t0 += m_buf_len;
  m_t1 += m_t0 < m_buf_len ? 1 : 0;

  for (std::size_t i = m_buf_len; i < BLOCK_BYTES; ++i) {
    m_buf[i] = std::byte{0};
  }

  compress(true);

  std::array<std::byte, MAX_DIGEST_BYTES> digest;
  for (int i = 0; i < 8; ++i) {
    store_le64(digest.data() + 8 * i, m_h[i]);
  }

  const auto n = std::min<std::size_t>(out.size(), m_digest_bytes);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = digest[i];
  }

  secure_zero(digest);
  wipe();
}

void Blake2b::wipe() noexcept {
  secure_zero(m_h);
  secure_zero(m_buf);
  m_t0 = 0;
  m_t1 = 0;
  m_buf_len = 0;
}

void Blake2b::hash(const std::span<std::byte> out, const std::span<const std::byte> in,
                   const std::span<const std::byte> key) noexcept {
  Blake2b state{out.size(), key};
  state.update(in);
  state.finalize(out);
}
}  // namespace kernel::crypto
