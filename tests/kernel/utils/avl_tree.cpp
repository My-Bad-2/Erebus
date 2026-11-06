#include "utils/avl_tree.hpp"

#include <array>
#include <cstdint>

#include "ktest.hpp"

namespace kernel::testing {
namespace {
struct Item : utils::AvlNode {
  std::uint32_t key{0};
  std::uint32_t order{0};
  std::uint32_t subtree_size{0};
};

struct Traits {
  [[nodiscard]] static bool less(const Item& a, const Item& b) noexcept { return a.key < b.key; }
  static void update(Item& item) noexcept;
};

using Tree = utils::AvlTree<Item, Traits>;

void Traits::update(Item& item) noexcept {
  const Item* left = Tree::left(item);
  const Item* right = Tree::right(item);
  item.subtree_size =
      1 + (left != nullptr ? left->subtree_size : 0) + (right != nullptr ? right->subtree_size : 0);
}

[[nodiscard]] std::uint32_t count_items(const Tree& tree) noexcept {
  std::uint32_t count = 0;
  for (const Item* item = tree.first(); item != nullptr; item = Tree::next(*item)) {
    ++count;
  }

  return count;
}
}  // namespace

KTEST(AvlTree, starts_out_empty) {
  const Tree tree;

  KEXPECT(tree.empty());
  KEXPECT(tree.root() == nullptr);
  KEXPECT(tree.first() == nullptr);
  KEXPECT(tree.last() == nullptr);
  KEXPECT(tree.is_consistent());
}

KTEST(AvlTree, knows_which_items_are_in_it) {
  Tree tree;
  Item item;

  KEXPECT(!item.in_tree());
  tree.insert(item);
  KEXPECT(item.in_tree());
  KEXPECT(tree.root() == &item);
  KEXPECT(tree.first() == &item);
  KEXPECT(tree.last() == &item);

  tree.erase(item);
  KEXPECT(!item.in_tree());
  KEXPECT(tree.empty());
}

KTEST(AvlTree, walks_in_order_both_ways) {
  Tree tree;
  std::array<Item, 64> items;

  // Going in out of order, to give the rotations something to do.
  for (std::uint32_t i = 0; i < items.size(); ++i) {
    items[i].key = (i * 37) % 64;
    tree.insert(items[i]);
  }

  std::uint32_t expected = 0;
  for (const Item* item = tree.first(); item != nullptr; item = Tree::next(*item)) {
    KEXPECT_EQ(item->key, expected++);
  }

  KEXPECT_EQ(expected, 64u);

  for (const Item* item = tree.last(); item != nullptr; item = Tree::prev(*item)) {
    KEXPECT_EQ(item->key, --expected);
  }

  KEXPECT_EQ(expected, 0u);
}

KTEST(AvlTree, sorted_input_stays_balanced) {
  Tree tree;
  static std::array<Item, 4096> items;

  for (std::uint32_t i = 0; i < items.size(); ++i) {
    items[i].key = i;
    tree.insert(items[i]);
  }

  KEXPECT(tree.is_consistent());

  // Everything to one side would make this 4095 on the way down the left.
  std::uint32_t depth = 0;
  for (const Item* item = tree.root(); item != nullptr; item = Tree::left(*item)) {
    ++depth;
  }

  KEXPECT_LE(depth, 18u);

  for (Item& item : items) {
    tree.erase(item);
  }

  KEXPECT(tree.empty());
}

KTEST(AvlTree, equal_items_stay_in_the_order_they_came) {
  Tree tree;
  std::array<Item, 32> items;

  for (std::uint32_t i = 0; i < items.size(); ++i) {
    items[i].order = i;
    tree.insert(items[i]);
  }

  std::uint32_t expected = 0;
  for (const Item* item = tree.first(); item != nullptr; item = Tree::next(*item)) {
    KEXPECT_EQ(item->order, expected++);
  }
}

KTEST(AvlTree, any_item_can_be_taken_out) {
  Tree tree;
  std::array<Item, 3> items;
  items[0].key = 1;
  items[1].key = 2;
  items[2].key = 3;

  // Root with two children, then with one, then alone.
  for (Item& item : items) {
    tree.insert(item);
  }

  KREQUIRE(tree.root() == &items[1]);
  tree.erase(items[1]);
  KEXPECT(tree.is_consistent());
  KEXPECT_EQ(count_items(tree), 2u);

  tree.erase(items[0]);
  KEXPECT(tree.is_consistent());
  KEXPECT(tree.first() == &items[2]);

  tree.erase(items[2]);
  KEXPECT(tree.empty());
}

KTEST(AvlTree, keeps_subtree_data_up_to_date_through_any_churn) {
  constexpr std::size_t COUNT = 512;
  static std::array<Item, COUNT> items;

  Tree tree;
  Rng rng{0xA71};
  std::uint32_t inside = 0;

  for (std::uint32_t step = 0; step < 30000; ++step) {
    Item& item = items[rng.below(COUNT)];

    if (item.in_tree()) {
      tree.erase(item);
      --inside;
    } else {
      item.key = static_cast<std::uint32_t>(rng.below(256));
      tree.insert(item);
      ++inside;
    }

    if (step % 32 == 0) {
      KREQUIRE(tree.is_consistent());

      // The top of the tree can only be right if every update on the way up happened.
      const Item* root = tree.root();
      KEXPECT_EQ(root != nullptr ? root->subtree_size : 0, inside);
      KEXPECT_EQ(count_items(tree), inside);
    }
  }

  for (Item& item : items) {
    if (item.in_tree()) {
      tree.erase(item);
    }
  }

  KEXPECT(tree.empty());
}
}  // namespace kernel::testing
