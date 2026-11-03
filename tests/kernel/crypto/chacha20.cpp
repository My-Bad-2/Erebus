#include "crypto/chacha20.hpp"

#include <algorithm>

#include "hex.hpp"
#include "ktest.hpp"

namespace kernel::testing::crypto {
namespace {
namespace chacha20 = kernel::crypto::chacha20;

[[nodiscard]] chacha20::Key counting_key() noexcept {
  chacha20::Key key;
  std::ranges::copy(counting(32), key.begin());
  return key;
}

[[nodiscard]] constexpr chacha20::Nonce nonce(const std::uint8_t b7,
                                              const std::uint8_t b3) noexcept {
  chacha20::Nonce n{};
  n[3] = static_cast<std::byte>(b3);
  n[7] = static_cast<std::byte>(b7);
  return n;
}

constexpr std::string_view SUNSCREEN =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
    "sunscreen would be it.";

std::byte g_stream[300];
std::byte g_part[300];
}  // namespace

// RFC 8439, 2.3.2.
KTEST(ChaCha20, matches_the_rfc8439_block_function) {
  std::byte out[64];
  chacha20::block(counting_key(), 1, nonce(0x4a, 0x09), out);
  KEXPECT_STREQ(Hex{out},
                "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
}

// RFC 8439, 2.4.2: the "sunscreen" example, 114 bytes over two blocks.
KTEST(ChaCha20, matches_the_rfc8439_encryption) {
  std::byte data[SUNSCREEN.size()];
  std::ranges::copy(bytes_of(SUNSCREEN), data);

  chacha20::xor_stream(counting_key(), 1, nonce(0x4a, 0), data);
  KEXPECT_STREQ(Hex{data},
                "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
                "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
                "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
                "5af90bbf74a35be6b40b8eedf2785e42874d");

  // Decrypting is the same operation.
  chacha20::xor_stream(counting_key(), 1, nonce(0x4a, 0), data);
  KEXPECT_BYTES_EQ(std::span<const std::byte>{data}, bytes_of(SUNSCREEN));
}

KTEST(ChaCha20, gives_the_same_keystream_whatever_the_length) {
  chacha20::keystream(counting_key(), 7, chacha20::Nonce{}, g_stream);

  for (const std::size_t n : {1u, 63u, 64u, 65u, 128u, 299u}) {
    chacha20::keystream(counting_key(), 7, chacha20::Nonce{}, std::span{g_part}.first(n));
    KEXPECT_BYTES_EQ(std::span{g_part}.first(n), std::span{g_stream}.first(n));
  }

  // Starting one block later skips exactly 64 bytes.
  chacha20::keystream(counting_key(), 8, chacha20::Nonce{}, std::span{g_part}.first(100));
  KEXPECT_BYTES_EQ(std::span{g_part}.first(100), std::span{g_stream}.subspan(64, 100));
}
}  // namespace kernel::testing::crypto
