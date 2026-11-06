/// @file avl_tree.hpp
/// @brief Intrusive height-balanced search tree.
///
/// Items derive from AvlNode and carry their own links, so the tree never allocates, and a thing
/// that goes into one can never fail to. That is what keeps it usable from an interrupt handler
/// or with a spinlock held, where the BPlusTree next door is not.
///
/// Every operation walks at most the height of the tree, which an AVL tree keeps to about 1.44
/// log2(n), and nothing is amortized: a worst case of a few dozen steps is one, with a million
/// items or at the moment that matters. Removing an item needs no search, since its node knows
/// where it is.
///
/// A tree can carry extra data in its nodes that depends on the node's subtree. After any change
/// below a node, the tree calls `Traits::update()` on it, children first, all the way to the root.
/// sched::EevdfQueue finds the earliest eligible deadline that way.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>

#include "utils/assert.hpp"

namespace kernel::utils {
template <typename T, typename Traits>
class AvlTree;

/// @brief The links an item needs to be in an AvlTree.
class AvlNode {
  template <typename T, typename Traits>
  friend class AvlTree;

  AvlNode* m_parent{nullptr};
  AvlNode* m_left{nullptr};
  AvlNode* m_right{nullptr};
  std::int8_t m_height{0};  // In the tree. 0 means not in one.

 public:
  constexpr AvlNode() noexcept = default;

  AvlNode(const AvlNode&) = delete;
  AvlNode& operator=(const AvlNode&) = delete;

  /// @brief Whether the item is in a tree right now.
  [[nodiscard]] constexpr bool in_tree() const noexcept { return m_height != 0; }
};

/// @brief What an AvlTree wants to know about its items.
template <typename Traits, typename T>
concept AvlTraits = requires(const T& a, const T& b, T& item) {
  { Traits::less(a, b) } noexcept -> std::same_as<bool>;
  { Traits::update(item) } noexcept -> std::same_as<void>;
};

/// @brief Intrusive search tree of @p T, which must derive from AvlNode.
///
/// @tparam Traits `static bool less(const T&, const T&)` gives the order, which must not change
///                while an item is in the tree. `static void update(T&)` recomputes whatever the
///                item keeps about its subtree; it can use left() and right(), whose updates are
///                done by then, and can be empty.
///
/// Items that compare equal stay in the order they went in. The tree does not own its items.
template <typename T, typename Traits>
class AvlTree {
  AvlNode* m_root{nullptr};

  [[nodiscard]] static constexpr T* item_of(AvlNode* node) noexcept {
    return static_cast<T*>(node);
  }

  [[nodiscard]] static constexpr int height(const AvlNode* node) noexcept {
    return node != nullptr ? node->m_height : 0;
  }

  static void refresh(AvlNode* node) noexcept {
    node->m_height =
        static_cast<std::int8_t>(1 + std::max(height(node->m_left), height(node->m_right)));
    Traits::update(*item_of(node));
  }

  void replace_child(const AvlNode* old_child, AvlNode* new_child) noexcept {
    AvlNode* parent = old_child->m_parent;
    if (parent == nullptr) {
      m_root = new_child;
    } else if (parent->m_left == old_child) {
      parent->m_left = new_child;
    } else {
      parent->m_right = new_child;
    }
  }

  AvlNode* rotate_left(AvlNode* node) noexcept {
    AvlNode* pivot = node->m_right;

    node->m_right = pivot->m_left;
    if (pivot->m_left != nullptr) {
      pivot->m_left->m_parent = node;
    }

    pivot->m_parent = node->m_parent;
    replace_child(node, pivot);
    pivot->m_left = node;
    node->m_parent = pivot;

    refresh(node);
    refresh(pivot);
    return pivot;
  }

  AvlNode* rotate_right(AvlNode* node) noexcept {
    AvlNode* pivot = node->m_left;

    node->m_left = pivot->m_right;
    if (pivot->m_right != nullptr) {
      pivot->m_right->m_parent = node;
    }

    pivot->m_parent = node->m_parent;
    replace_child(node, pivot);
    pivot->m_right = node;
    node->m_parent = pivot;

    refresh(node);
    refresh(pivot);
    return pivot;
  }

  // Always climbs to the root: what update() keeps can change all the way up, unlike the height.
  void rebalance_upwards(AvlNode* node) noexcept {
    while (node != nullptr) {
      refresh(node);

      const int balance = height(node->m_left) - height(node->m_right);
      if (balance > 1) {
        KASSERT(node->m_left != nullptr);  // Taller than the other side, so it is there.
        if (height(node->m_left->m_left) < height(node->m_left->m_right)) {
          rotate_left(node->m_left);
        }

        node = rotate_right(node);
      } else if (balance < -1) {
        KASSERT(node->m_right != nullptr);
        if (height(node->m_right->m_right) < height(node->m_right->m_left)) {
          rotate_right(node->m_right);
        }

        node = rotate_left(node);
      }

      node = node->m_parent;
    }
  }

  [[nodiscard]] bool is_subtree_consistent(const AvlNode* node) const noexcept {
    if (node == nullptr) {
      return true;
    }

    const AvlNode* left = node->m_left;
    const AvlNode* right = node->m_right;

    const bool links_hold = (left == nullptr || left->m_parent == node) &&
                            (right == nullptr || right->m_parent == node);
    const bool order_holds = (left == nullptr || !Traits::less(*static_cast<const T*>(node),
                                                               *static_cast<const T*>(left))) &&
                             (right == nullptr || !Traits::less(*static_cast<const T*>(right),
                                                                *static_cast<const T*>(node)));

    const int balance = height(left) - height(right);
    const bool balance_holds = balance >= -1 && balance <= 1 &&
                               node->m_height == 1 + std::max(height(left), height(right));

    return links_hold && order_holds && balance_holds && is_subtree_consistent(left) &&
           is_subtree_consistent(right);
  }

 public:
  constexpr AvlTree() noexcept = default;

  AvlTree(const AvlTree&) = delete;
  AvlTree& operator=(const AvlTree&) = delete;

  [[nodiscard]] constexpr bool empty() const noexcept { return m_root == nullptr; }

  /// @brief The item at the top, for walks of your own with left() and right().
  [[nodiscard]] constexpr T* root() const noexcept { return item_of(m_root); }

  [[nodiscard]] static constexpr T* left(const T& item) noexcept {
    return item_of(static_cast<const AvlNode&>(item).m_left);
  }

  [[nodiscard]] static constexpr T* right(const T& item) noexcept {
    return item_of(static_cast<const AvlNode&>(item).m_right);
  }

  /// @brief Puts @p item in the tree, behind any it compares equal to.
  void insert(T& item) noexcept {
    AvlNode* node = &item;
    AvlNode* parent = nullptr;
    AvlNode** link = &m_root;

    while (*link != nullptr) {
      parent = *link;
      link = Traits::less(item, *item_of(parent)) ? &parent->m_left : &parent->m_right;
    }

    node->m_parent = parent;
    node->m_left = nullptr;
    node->m_right = nullptr;
    *link = node;

    rebalance_upwards(node);
  }

  /// @brief Takes @p item out of the tree.
  void erase(T& item) noexcept {
    AvlNode* node = &item;
    AvlNode* retrace_from = nullptr;

    if (node->m_left != nullptr && node->m_right != nullptr) {
      // Two children: the next node in order takes this one's place.
      AvlNode* next = node->m_right;
      while (next->m_left != nullptr) {
        next = next->m_left;
      }

      if (next->m_parent == node) {
        retrace_from = next;
      } else {
        retrace_from = next->m_parent;
        retrace_from->m_left = next->m_right;
        if (next->m_right != nullptr) {
          next->m_right->m_parent = retrace_from;
        }

        next->m_right = node->m_right;
        next->m_right->m_parent = next;
      }

      next->m_left = node->m_left;
      next->m_left->m_parent = next;
      next->m_parent = node->m_parent;
      replace_child(node, next);
    } else {
      AvlNode* child = node->m_left != nullptr ? node->m_left : node->m_right;
      if (child != nullptr) {
        child->m_parent = node->m_parent;
      }

      replace_child(node, child);
      retrace_from = node->m_parent;
    }

    node->m_parent = nullptr;
    node->m_left = nullptr;
    node->m_right = nullptr;
    node->m_height = 0;

    rebalance_upwards(retrace_from);
  }

  /// @brief The lowest item, or null.
  [[nodiscard]] T* first() const noexcept {
    AvlNode* node = m_root;
    while (node != nullptr && node->m_left != nullptr) {
      node = node->m_left;
    }

    return item_of(node);
  }

  /// @brief The highest item, or null.
  [[nodiscard]] T* last() const noexcept {
    AvlNode* node = m_root;
    while (node != nullptr && node->m_right != nullptr) {
      node = node->m_right;
    }

    return item_of(node);
  }

  /// @brief The item right after @p item, or null.
  [[nodiscard]] static T* next(const T& item) noexcept {
    const AvlNode* node = &item;

    if (node->m_right != nullptr) {
      AvlNode* lowest = node->m_right;
      while (lowest->m_left != nullptr) {
        lowest = lowest->m_left;
      }

      return item_of(lowest);
    }

    AvlNode* parent = node->m_parent;
    while (parent != nullptr && parent->m_right == node) {
      node = parent;
      parent = parent->m_parent;
    }

    return item_of(parent);
  }

  /// @brief The item right before @p item, or null.
  [[nodiscard]] static T* prev(const T& item) noexcept {
    const AvlNode* node = &item;

    if (node->m_left != nullptr) {
      AvlNode* highest = node->m_left;
      while (highest->m_right != nullptr) {
        highest = highest->m_right;
      }

      return item_of(highest);
    }

    AvlNode* parent = node->m_parent;
    while (parent != nullptr && parent->m_left == node) {
      node = parent;
      parent = parent->m_parent;
    }

    return item_of(parent);
  }

  /// @brief Checks every invariant of the tree. O(n), for tests.
  [[nodiscard]] bool is_consistent() const noexcept {
    return (m_root == nullptr || m_root->m_parent == nullptr) && is_subtree_consistent(m_root);
  }
};
}  // namespace kernel::utils
