#include <cstddef>
#include <cstdint>

#include "string.h"

namespace klib {
namespace {
// A 256-bit set of byte values, built once per call so the scan costs one bit test per byte.
class ByteSet {
  std::uint64_t m_bits[4]{};

 public:
  constexpr void add(const unsigned char c) noexcept {
    m_bits[c >> 6] |= std::uint64_t{1} << (c & 63);
  }

  [[nodiscard]] constexpr bool contains(const unsigned char c) const noexcept {
    return (m_bits[c >> 6] >> (c & 63)) & 1;
  }
};

// Length of the leading run of `str` whose bytes are all in `chars` (Accept) or all outside it.
template <bool Accept>
std::size_t span(const char* str, const char* chars) noexcept {
  ByteSet set;
  for (auto* c = reinterpret_cast<const unsigned char*>(chars); *c != 0; ++c) {
    set.add(*c);
  }

  // The terminator is never in the set, so it ends an accepting run on its own; for a rejecting
  // run, putting it in the set does the same.
  if constexpr (!Accept) {
    set.add(0);
  }

  const auto* s = reinterpret_cast<const unsigned char*>(str);
  std::size_t len = 0;
  while (set.contains(s[len]) == Accept) {
    ++len;
  }

  return len;
}
}  // namespace

std::size_t strspn(const char* str, const char* accept) noexcept { return span<true>(str, accept); }
std::size_t strcspn(const char* str, const char* reject) noexcept {
  return span<false>(str, reject);
}
}  // namespace klib
