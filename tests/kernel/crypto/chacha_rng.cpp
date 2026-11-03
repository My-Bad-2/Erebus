#include <algorithm>
#include <array>

#include "crypto/chacha20.hpp"
#include "hex.hpp"
#include "ktest.hpp"

// Expected values: the same construction worked by hand in Python on top of the `cryptography`
// package's ChaCha20.
namespace kernel::testing::crypto {
namespace {
using kernel::crypto::ChaChaRng;
namespace chacha20 = kernel::crypto::chacha20;

[[nodiscard]] ChaChaRng seeded() noexcept {
  chacha20::Key key;
  std::ranges::copy(counting(32), key.begin());
  ChaChaRng rng;
  rng.reseed(key);
  return rng;
}

[[nodiscard]] Hex take(ChaChaRng& rng, const std::size_t n) noexcept {
  std::byte out[256]{};
  rng.fill(std::span{out}.first(n));
  return Hex{std::span{out}.first(n)};
}

// Concatenated hex of a few small draws.
struct Joined {
  char text[2 * 256 + 1]{};
  std::size_t length = 0;

  void append(const Hex& hex) noexcept {
    for (const char c : std::string_view{hex}) {
      text[length++] = c;
    }
  }
};

// 10,000 draws of 16 bytes, to look for repeats.
std::array<std::uint64_t, 2> g_draws[10'000];
}  // namespace

KTEST(ChaChaRng, matches_fast_key_erasure_by_hand) {
  ChaChaRng rng = seeded();
  // The second half of the first block of the seed's stream...
  KEXPECT_STREQ(take(rng, 32), "2b23cce7a26023ab3f0eef693ac87f64258235eab1f7a32dc22762a0485b410c");
  // ...then the second half of the first block of the key its first half became.
  KEXPECT_STREQ(take(rng, 32), "2d41a59c90e41a8e7a4dccaa1c46069983b1a333ce25719ec3437768ab57fa42");
}

KTEST(ChaChaRng, continues_the_stream_for_large_requests) {
  ChaChaRng rng = seeded();
  KEXPECT_STREQ(take(rng, 200),
                "2b23cce7a26023ab3f0eef693ac87f64258235eab1f7a32dc22762a0485b410c"
                "18b84231ade6a6d113615c61af434e27f8b1f3f5e1ad5b5cecf8fc122a35755c"
                "7208086dd1ee3c5d9d815824640e003c9ba0f65ede5d59ce0d2a4a7f31955acd"
                "42f22ddca74a92d56ca78aef298e723b60237f3647eabeb7f3e09c30ce80e3e2"
                "84a8021b8a5c0b2494cd3c8d5b13507ec7e7a0784df4a3e2ea8162d261c59d23"
                "e7ab11c0f73c3b7eb0983950b3e2c4a08f843da95fb7fcb3f13456816b51b782"
                "4df2f9bd5613d4b4");

  // The key moved on all the same.
  KEXPECT_STREQ(take(rng, 32), "2d41a59c90e41a8e7a4dccaa1c46069983b1a333ce25719ec3437768ab57fa42");
}

KTEST(ChaChaRng, serves_small_requests_from_what_is_left) {
  ChaChaRng whole = seeded();
  ChaChaRng pieces = seeded();
  Joined joined;
  joined.append(take(pieces, 5));
  joined.append(take(pieces, 20));
  joined.append(take(pieces, 7));
  KEXPECT_STREQ(joined.text, take(whole, 32));
}

KTEST(ChaChaRng, forgets_buffered_output_when_reseeded) {
  ChaChaRng rng = seeded();
  (void)take(rng, 4);

  chacha20::Key other{};
  other[0] = std::byte{1};
  rng.reseed(other);
  ChaChaRng fresh;
  fresh.reseed(other);
  KEXPECT_STREQ(take(rng, 48), take(fresh, 48));
}

KTEST(ChaChaRng, does_not_repeat_itself) {
  ChaChaRng rng = seeded();
  for (auto& draw : g_draws) {
    rng.fill(std::as_writable_bytes(std::span{draw}));
  }

  std::ranges::sort(g_draws);
  for (std::size_t i = 1; i < std::size(g_draws); ++i) {
    KREQUIRE(g_draws[i] != g_draws[i - 1]);
  }
}
}  // namespace kernel::testing::crypto
