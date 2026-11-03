#include "crypto/chacha20.hpp"

#include <algorithm>
#include <bit>

#include "crypto/bytes.hpp"

namespace kernel::crypto {
namespace chacha20 {
namespace {
using State = std::array<std::uint32_t, 16>;

// "expand 32-byte k"
constexpr std::uint32_t SIGMA[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};

[[gnu::always_inline]] inline void quarter_round(State& x, const int a, const int b, const int c,
                                                 const int d) noexcept {
  x[a] += x[b];
  x[d] = std::rotl(x[d] ^ x[a], 16);
  x[c] += x[d];
  x[b] = std::rotl(x[b] ^ x[c], 12);
  x[a] += x[b];
  x[d] = std::rotl(x[d] ^ x[a], 8);
  x[c] += x[d];
  x[b] = std::rotl(x[b] ^ x[c], 7);
}

State initial_state(const Key& key, const std::uint32_t counter, const Nonce& nonce) noexcept {
  State s;
  for (int i = 0; i < 4; ++i) {
    s[i] = SIGMA[i];
  }

  for (int i = 0; i < 8; ++i) {
    s[4 + i] = load_le32(key.data() + 4 * i);
  }

  s[12] = counter;
  for (int i = 0; i < 3; ++i) {
    s[13 + i] = load_le32(nonce.data() + 4 * i);
  }

  return s;
}

// One block from `input`, which then moves on to the next counter value.
void next_block(State& input, const std::span<std::byte, BLOCK_BYTES> out) noexcept {
  State x = input;
  for (int round = 0; round < 10; ++round) {
    quarter_round(x, 0, 4, 8, 12);
    quarter_round(x, 1, 5, 9, 13);
    quarter_round(x, 2, 6, 10, 14);
    quarter_round(x, 3, 7, 11, 15);
    quarter_round(x, 0, 5, 10, 15);
    quarter_round(x, 1, 6, 11, 12);
    quarter_round(x, 2, 7, 8, 13);
    quarter_round(x, 3, 4, 9, 14);
  }

  for (int i = 0; i < 16; ++i) {
    store_le32(out.data() + 4 * i, x[i] + input[i]);
  }

  ++input[12];
  secure_zero(x);
}

template <bool Xor>
void stream(const Key& key, const std::uint32_t counter, const Nonce& nonce,
            std::span<std::byte> data) noexcept {
  State state = initial_state(key, counter, nonce);
  std::array<std::byte, BLOCK_BYTES> block_bytes;

  while (!data.empty()) {
    const std::size_t n = std::min(data.size(), BLOCK_BYTES);
    if (!Xor && n == BLOCK_BYTES) {
      next_block(state, data.first<BLOCK_BYTES>());
    } else {
      next_block(state, block_bytes);
      for (std::size_t i = 0; i < n; ++i) {
        data[i] = Xor ? data[i] ^ block_bytes[i] : block_bytes[i];
      }
    }

    data = data.subspan(n);
  }

  secure_zero(state);
  secure_zero(block_bytes);
}
}  // namespace

void block(const Key& key, const std::uint32_t counter, const Nonce& nonce,
           const std::span<std::byte, BLOCK_BYTES> out) noexcept {
  State state = initial_state(key, counter, nonce);
  next_block(state, out);
  secure_zero(state);
}

void keystream(const Key& key, const std::uint32_t counter, const Nonce& nonce,
               const std::span<std::byte> out) noexcept {
  stream<false>(key, counter, nonce, out);
}

void xor_stream(const Key& key, const std::uint32_t counter, const Nonce& nonce,
                const std::span<std::byte> data) noexcept {
  stream<true>(key, counter, nonce, data);
}
}  // namespace chacha20

namespace {
// Every key is used for one stream only, so a fixed nonce is fine.
constexpr chacha20::Nonce RNG_NONCE{};
}  // namespace

void ChaChaRng::take_buffered(std::span<std::byte>& out) noexcept {
  const std::size_t n = std::min<std::size_t>(out.size(), m_available);
  if (n == 0) {
    return;
  }

  const std::span<std::byte> taken = std::span{m_buffer}.subspan(m_buffer.size() - m_available, n);
  std::ranges::copy(taken, out.begin());
  secure_zero(taken);

  m_available -= static_cast<std::uint32_t>(n);
  out = out.subspan(n);
}

void ChaChaRng::reseed(const chacha20::Key& seed) noexcept {
  m_key = seed;
  secure_zero(m_buffer);
  m_available = 0;
}

void ChaChaRng::fill(std::span<std::byte> out) noexcept {
  take_buffered(out);
  if (out.empty()) {
    return;
  }

  // The first block of the current key's stream: its first half replaces the key at once.
  chacha20::Key key = m_key;
  std::array<std::byte, chacha20::BLOCK_BYTES> first;
  chacha20::block(key, 0, RNG_NONCE, first);
  std::ranges::copy(std::span{first}.first<chacha20::KEY_BYTES>(), m_key.begin());

  const auto output = std::span{first}.subspan<chacha20::KEY_BYTES>();
  if (out.size() <= output.size()) {
    // A small request: keep what it leaves of the block for the next one.
    std::ranges::copy(output.first(out.size()), out.begin());
    m_available = static_cast<std::uint32_t>(output.size() - out.size());
    std::ranges::copy(output.last(m_available), m_buffer.end() - m_available);
  } else {
    // A large one gets the rest of the old key's stream as well.
    std::ranges::copy(output, out.begin());
    chacha20::keystream(key, 1, RNG_NONCE, out.subspan(output.size()));
  }

  secure_zero(first);
  secure_zero(key);
}

void ChaChaRng::wipe() noexcept {
  secure_zero(m_key);
  secure_zero(m_buffer);
  m_available = 0;
}
}  // namespace kernel::crypto
