#include "crypto/blake2b.hpp"

#include "hex.hpp"
#include "ktest.hpp"

// Reference digests computed with Python's hashlib.blake2b.
namespace kernel::testing::crypto {
namespace {
using kernel::crypto::Blake2b;

[[nodiscard]] Hex digest(const std::span<const std::byte> in, const std::size_t n = 64,
                         const std::span<const std::byte> key = {}) noexcept {
  std::byte out[64]{};
  Blake2b::hash(std::span{out}.first(n), in, key);
  return Hex{std::span{out}.first(n)};
}

// A megabyte of 'a', fed in 4 KiB pieces.
std::byte g_megabyte[4096];
}  // namespace

KTEST(Blake2b, matches_rfc7693) {
  KEXPECT_STREQ(digest(bytes_of("abc")),
                "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
                "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
  KEXPECT_STREQ(digest({}),
                "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
                "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
}

KTEST(Blake2b, keyed_matches_the_reference) {
  const auto key = counting(64);
  KEXPECT_STREQ(digest({}, 64, key),
                "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786"
                "b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568");
  KEXPECT_STREQ(digest(counting(255), 64, key),
                "142709d62e28fcccd0af97fad0f8465b971e82201dc51070faa0372aa43e9248"
                "4be1c1e73ba10906d5d1853db6a4106e0a7bf9800d373d6dee2d46d62ef2a461");
  KEXPECT_STREQ(digest(bytes_of("erebus"), 1, bytes_of("k")), "6f");
}

KTEST(Blake2b, short_digests_are_not_truncated_long_ones) {
  KEXPECT_STREQ(digest(bytes_of("The quick brown fox jumps over the lazy dog"), 32),
                "01718cec35cd3d796dd00020e0bfecb473ad23457d063b75eff29c0ffa2e58a9");
}

// A message of exactly one block must be compressed as the last one, not followed by an empty
// block.
KTEST(Blake2b, handles_block_boundaries) {
  KEXPECT_STREQ(digest(counting(128)),
                "2319e3789c47e2daa5fe807f61bec2a1a6537fa03f19ff32e87eecbfd64b7e0e"
                "8ccff439ac333b040f19b0c4ddd11a61e24ac1fe0f10a039806c5dcc0da3d115");
  KEXPECT_STREQ(digest(counting(129)),
                "f59711d44a031d5f97a9413c065d1e614c417ede998590325f49bad2fd444d3e"
                "4418be19aec4e11449ac1a57207898bc57d76a1bcf3566292c20c683a5c4648f");
}

KTEST(Blake2b, gives_the_same_digest_however_the_input_is_split) {
  const auto data = counting(300);
  const Hex expected = digest(data);

  for (std::size_t split = 0; split <= data.size(); split += 7) {
    Blake2b state;
    state.update(data.first(split));
    state.update(data.subspan(split));
    std::byte out[64]{};
    state.finalize(out);
    KREQUIRE_STREQ(Hex{out}, expected);
  }
}

KTEST(Blake2b, hashes_a_megabyte) {
  for (std::byte& b : g_megabyte) {
    b = std::byte{'a'};
  }

  Blake2b state;
  constexpr std::size_t TOTAL = 1'000'000;
  for (std::size_t done = 0; done < TOTAL; done += sizeof(g_megabyte)) {
    const std::size_t n = TOTAL - done < sizeof(g_megabyte) ? TOTAL - done : sizeof(g_megabyte);
    state.update(std::span{g_megabyte}.first(n));
  }

  std::byte out[64]{};
  state.finalize(out);
  KEXPECT_STREQ(Hex{out},
                "98fb3efb7206fd19ebf69b6f312cf7b64e3b94dbe1a17107913975a793f177e1"
                "d077609d7fba363cbba00d05f7aa4e4fa8715d6428104c0a75643b0ff3fd3eaf");
}

KTEST(Blake2b, a_copy_finalizes_a_snapshot) {
  Blake2b running;
  running.update(bytes_of("ab"));
  Blake2b snapshot = running;
  running.update(bytes_of("c"));

  std::byte first[64]{};
  std::byte second[64]{};
  snapshot.finalize(first);
  running.finalize(second);
  KEXPECT_STREQ(Hex{first}, digest(bytes_of("ab")));
  KEXPECT_STREQ(Hex{second}, digest(bytes_of("abc")));
}

KTEST(Blake2b, clamps_out_of_range_sizes) {
  KEXPECT_EQ(Blake2b{0}.digest_bytes(), 1u);
  KEXPECT_EQ(Blake2b{1000}.digest_bytes(), 64u);
}
}  // namespace kernel::testing::crypto
