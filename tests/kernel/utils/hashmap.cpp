#include "utils/hashmap.hpp"

#include "ktest.hpp"

namespace kernel::testing {
namespace {
constexpr std::uint64_t KEYS = 8192;
std::uint64_t g_objects[KEYS];
bool g_present[KEYS];

// Page-aligned keys: the low twelve bits never change, which a weak hash would choke on.
[[nodiscard]] constexpr std::uint64_t key_of(const std::uint64_t i) noexcept {
  return (i + 1) << 12;
}
}  // namespace

KTEST(FastHashMap, agrees_with_a_model_on_random_operations) {
  for (bool& present : g_present) {
    present = false;
  }

  utils::FastHashMap<std::uint64_t, std::uint64_t, 4> map;
  Rng rng{0x4A5B};
  std::size_t count = 0;

  for (std::uint32_t round = 0; round < 200000; ++round) {
    const std::uint64_t i = rng.below(KEYS);
    const std::uint64_t key = key_of(i);

    switch (rng.below(3)) {
      case 0:
        KREQUIRE_EQ(map.insert(key, &g_objects[i]), !g_present[i]);
        count += g_present[i] ? 0 : 1;
        g_present[i] = true;
        break;
      case 1:
        KREQUIRE_EQ(map.erase(key), g_present[i] ? &g_objects[i] : nullptr);
        count -= g_present[i] ? 1 : 0;
        g_present[i] = false;
        break;
      default:
        KREQUIRE_EQ(map.lookup(key), g_present[i] ? &g_objects[i] : nullptr);
        break;
    }
  }

  KEXPECT_EQ(map.size(), count);

  std::size_t visited = 0;
  bool matches = true;
  map.for_each([&](const std::uint64_t key, std::uint64_t* object) {
    const std::uint64_t i = (key >> 12) - 1;
    matches = matches && i < KEYS && g_present[i] && object == &g_objects[i];
    ++visited;
  });
  KEXPECT(matches);
  KEXPECT_EQ(visited, count);
}
}  // namespace kernel::testing
