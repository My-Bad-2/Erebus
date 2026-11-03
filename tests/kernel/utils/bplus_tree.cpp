#include "utils/bplus_tree.hpp"

#include "ktest.hpp"

namespace kernel::testing {
namespace {
constexpr std::uint64_t KEYS = 4096;

// The reference model: which keys the tree should hold, and their values.
bool g_present[KEYS];
std::uint64_t g_values[KEYS];

// Small nodes make the tree deep, so splits, borrows and merges all get plenty of exercise.
using Tree = utils::BPlusTree<std::uint64_t, std::uint64_t, 128, utils::NullLock>;

// Checks size, lookups, floor lookups and in-order iteration against the model.
void verify(Tree& tree, const std::size_t count) noexcept {
  KEXPECT_EQ(tree.size(), count);

  std::uint64_t floor_key = 0;
  bool any = false;
  bool lookups_agree = true;
  for (std::uint64_t k = 0; k < KEYS; ++k) {
    std::uint64_t value = 0;
    lookups_agree = lookups_agree && tree.get(k, value) == g_present[k] &&
                    (!g_present[k] || value == g_values[k]);

    if (g_present[k]) {
      floor_key = k;
      any = true;
    }

    std::uint64_t fk = 0;
    std::uint64_t fv = 0;
    lookups_agree = lookups_agree && tree.get_floor(k, fk, fv) == any &&
                    (!any || (fk == floor_key && fv == g_values[fk]));
  }
  KEXPECT(lookups_agree);

  std::uint64_t prev = 0;
  std::size_t seen = 0;
  bool ordered = true;
  tree.range(0, KEYS, [&](const std::uint64_t k, const std::uint64_t value) {
    ordered = ordered && g_present[k] && value == g_values[k] && (seen == 0 || k > prev);
    prev = k;
    ++seen;
    return true;
  });
  KEXPECT(ordered);
  KEXPECT_EQ(seen, count);
}
}  // namespace

KTEST(BPlusTree, agrees_with_a_model_while_growing_and_shrinking) {
  for (std::uint64_t k = 0; k < KEYS; ++k) {
    g_present[k] = false;
  }

  Tree tree;
  Rng rng{0xB7EE};
  std::size_t count = 0;

  for (std::uint32_t round = 0; round < 60000; ++round) {
    const std::uint64_t k = rng.below(KEYS);
    // Lean towards inserting first, then towards erasing, so the tree grows and shrinks again.
    const bool grow = round < 30000 ? rng.below(4) != 0 : rng.below(4) == 0;

    if (grow) {
      const std::uint64_t value = rng.next();
      KREQUIRE(tree.insert(k, value));
      count += g_present[k] ? 0 : 1;
      g_present[k] = true;
      g_values[k] = value;
    } else {
      KREQUIRE_EQ(tree.erase(k), g_present[k]);
      count -= g_present[k] ? 1 : 0;
      g_present[k] = false;
    }

    if (round % 10000 == 9999) {
      verify(tree, count);
    }
  }

  for (std::uint64_t k = 0; k < KEYS; ++k) {
    KREQUIRE_EQ(tree.erase(k), g_present[k]);
    g_present[k] = false;
  }

  verify(tree, 0);
  KEXPECT(tree.empty());
}
}  // namespace kernel::testing
