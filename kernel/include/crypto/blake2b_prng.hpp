#pragma once

#include "hal/hal.hpp"
#include <array>
#include <cstdint>
#include <string.h>

namespace kernel::crypto {
class Blake2bPrng {
  alignas(std::hardware_destructive_interference_size) std::array<std::uint64_t, 16> m_state{};
  alignas(std::hardware_destructive_interference_size) std::array<std::byte, 64> m_buffer{};

  std::size_t m_buf_idx{64};

  static consteval std::array<uint64_t, 8> get_iv() noexcept {
    return {
        0x6a09e667f3bcc908ul, 0xbb67ae8584caa73bul, 0x3c6ef372fe94f82bul, 0xa54ff53a5f1d36f1ul,
        0x510e527fade682d1ul, 0x9b05688c2b3e6c1ful, 0x1f83d9abfb41bd6bul, 0x5be0cd19137e2179ul,
    };
  }

  [[nodiscard]] static std::uint64_t extract_entropy() noexcept {
    std::uint8_t success;
    std::uint64_t seed;

    for (int i = 0; i < 10; ++i) {
      asm volatile("rdseed %0\n\t"
                   "setc %1\n\t"
                   : "=r"(seed), "=qm"(success)
                   : /* No Input */
                   : /* No Clobber */);
      if (success) {
        return seed;
      }

      hw::pause();
    }

    for (int i = 0; i < 10; ++i) {
      asm volatile("rdrand %0\n\t"
                   "setc %1\n\t"
                   : "=r"(seed), "=qm"(success)
                   : /* No Input */
                   : /* No Clobber */);
      if (success) {
        return seed;
      }
    }

    std::uint64_t tsc = hw::read_tsc();
    return tsc ^ std::rotr(tsc, 17);
  }

  template <std::unsigned_integral T>
    requires std::same_as<T, std::uint64_t>
  [[gnu::always_inline]] static constexpr void mix(T &a, T &b, T &c, T &d) noexcept {
    a += b;
    d = std::rotr(d ^ a, 32);
    c += d;
    b = std::rotr(b ^ c, 24);
    a += b;
    d = std::rotr(d ^ a, 16);
    c += d;
    b = std::rotr(b ^ c, 63);
  }

  void generate_block(std::span<std::byte> direct_out = {}) noexcept {
    alignas(std::hardware_constructive_interference_size) std::array<std::uint64_t, 16> v = m_state;

#pragma GCC unroll 12
    for (int i = 0; i < 12; ++i) {
      mix(v[0], v[4], v[8], v[12]);
      mix(v[1], v[5], v[9], v[13]);
      mix(v[2], v[6], v[10], v[14]);
      mix(v[3], v[7], v[11], v[15]);

      mix(v[0], v[5], v[10], v[15]);
      mix(v[1], v[6], v[11], v[12]);
      mix(v[2], v[7], v[8], v[13]);
      mix(v[3], v[4], v[9], v[14]);
    }

    // overwrite the key/nonce (words 0-7)
    for (std::size_t i = 0; i < 8; ++i) {
      m_state[i] = v[i] + m_state[i];
    }

    // increment the 128-bit counter
    if (++m_state[12] == 0) [[unlikely]] {
      ++m_state[13];
    }

    if (direct_out.size() >= 64) {
      for (std::size_t i = 8; i < 16; ++i) {
        const std::uint64_t out_word = v[i] + m_state[i];
        klib::memcpy(direct_out.data() + (i - 8) * 8, &out_word, 8);
      }

      m_buf_idx = 64;
    } else {
      for (std::size_t i = 8; i < 16; ++i) {
        const uint64_t out_word = v[i] + m_state[i];
        klib::memcpy(m_buffer.data() + (i - 8) * 8, &out_word, 8);
      }

      m_buf_idx = 0;
    }

    std::ranges::fill(v, 0);
  }

  Blake2bPrng() noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
      m_state[i] = extract_entropy();
    }

    constexpr auto iv = get_iv();
    std::ranges::copy(iv, m_state.begin() + 8);
    m_state[12] = 0;
    m_state[13] = 0;
  }

public:
  Blake2bPrng(const Blake2bPrng &) = delete;
  Blake2bPrng &operator=(const Blake2bPrng &) = delete;

  [[nodiscard]] static Blake2bPrng create() noexcept { return Blake2bPrng{}; }

  void inject_entropy(const std::uint64_t environmental_noise) noexcept {
    m_state[0] ^= environmental_noise;
    m_state[1] ^= extract_entropy();
    m_buf_idx = 64;
  }

  void get_random_bytes(std::span<std::byte> out) noexcept {
    [[assume(m_buf_idx <= 64)]];

    while (!out.empty()) {
      const std::size_t avail = 64 - m_buf_idx;

      if (avail == 0 && out.size() >= 64) {
        generate_block(out);
        out = out.subspan(64);
        continue;
      }

      if (avail == 0) {
        generate_block();
        continue;
      }

      const std::size_t to_copy = std::min(out.size(), avail);
      klib::memcpy(out.data(), m_buffer.data() + m_buf_idx, to_copy);

      m_buf_idx += to_copy;
      out = out.subspan(to_copy);
    }
  }

  template <typename T>
    requires std::is_integral_v<T>
  void get_random(T &out) noexcept {
    get_random_bytes(std::as_writable_bytes(std::span{&out, 1}));
  }
};
} // namespace kernel::crypto