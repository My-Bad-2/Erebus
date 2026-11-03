// Hex helpers for the crypto tests.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace kernel::testing::crypto {
// Hex of `bytes` into `out`, terminated; long enough for a 64-byte digest and then some.
struct Hex {
  char text[2 * 256 + 1]{};

  explicit Hex(const std::span<const std::byte> bytes) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    std::size_t at = 0;
    for (const std::byte b : bytes.first(bytes.size() < 256 ? bytes.size() : 256)) {
      text[at++] = digits[std::to_integer<unsigned>(b) >> 4];
      text[at++] = digits[std::to_integer<unsigned>(b) & 0xF];
    }
  }

  [[nodiscard]] operator std::string_view() const noexcept { return text; }
};

[[nodiscard]] inline std::span<const std::byte> bytes_of(const std::string_view text) noexcept {
  return std::as_bytes(std::span{text.data(), text.size()});
}

// Bytes 0, 1, 2, ... up to `count`, at most 512 of them.
[[nodiscard]] inline std::span<const std::byte> counting(const std::size_t count) noexcept {
  static constexpr auto table = [] {
    struct Table {
      std::byte bytes[512];
    } t{};
    for (std::size_t i = 0; i < 512; ++i) {
      t.bytes[i] = static_cast<std::byte>(i);
    }
    return t;
  }();
  return std::span{table.bytes}.first(count);
}
}  // namespace kernel::testing::crypto
