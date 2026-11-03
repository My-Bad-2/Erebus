/// @file bplus_tree.hpp
/// @brief Ordered map with cache-line sized nodes.

#pragma once

#include <string.h>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "memory/heap.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/maths.hpp"

namespace kernel::utils {
template <typename T>
concept BTreeData = requires(T a, T b) {
  { a < b } -> std::same_as<bool>;
  { a == b } -> std::same_as<bool>;
} && std::is_trivially_copyable_v<T>;

/// @brief B+ tree from @p K to @p V, for small trivially copyable keys and values.
///
/// Nodes are sized to @p NodeBytes, so a lookup touches one cache line (or a few) per level.
/// Values live in the leaves only, and the leaves are linked both ways for range walks and
/// floor lookups. Every node except the root is kept at least half full: inserts split full
/// nodes, erases borrow from a sibling or merge with it.
///
/// Inserts never fail half way: every node a split may need is allocated before the tree is
/// touched, so running out of memory leaves it unchanged. Nodes come from one slab cache shared
/// by every tree with the same node layout, and an empty tree allocates nothing.
///
/// @tparam Lock Protects the tree. RWLock by default; NullLock for trees that are only ever
///              used under a lock their owner already holds.
template <BTreeData K, BTreeData V, size_t NodeBytes = std::hardware_destructive_interference_size,
          typename Lock = RWLock>
class BPlusTree {
  static consteval std::size_t calc_max_keys() {
    for (std::size_t n = 64; n >= 3; --n) {
      std::size_t offset = sizeof(uint16_t);  // Header

      offset = maths::align_up(offset, alignof(K));
      offset += n * sizeof(K);

      const std::size_t children_size = (n + 1) * sizeof(void*);
      const std::size_t leaf_data_size =
          n * sizeof(V) + 2 * sizeof(void*);  // values + next_leaf + prev_leaf

      const std::size_t union_size =
          (children_size > leaf_data_size) ? children_size : leaf_data_size;
      const std::size_t union_align = (alignof(V) > alignof(void*)) ? alignof(V) : alignof(void*);

      offset = maths::align_up(offset, union_align);

      if (offset + union_size <= NodeBytes) {
        return n;
      }
    }

    return 0;
  }

  // Room in a node. A node is split as soon as it fills up, so between operations it holds at
  // most MAX_KEYS; the extra slot is what lets an insert land before the split.
  static constexpr size_t MAX_PHYSICAL_KEYS = calc_max_keys();
  static_assert(MAX_PHYSICAL_KEYS >= 3, "NodeBytes too small to satisfy minimum branching factor.");

  static constexpr std::size_t MAX_KEYS = MAX_PHYSICAL_KEYS - 1;
  // Small enough that a node one key short of this and a sibling at it fit in one node together,
  // with the separator from the parent for internal nodes.
  static constexpr std::size_t MIN_KEYS = MAX_KEYS / 2;
  static_assert(MIN_KEYS >= 1 && 2 * MIN_KEYS <= MAX_KEYS);

  struct alignas(std::hardware_destructive_interference_size) Node {
    std::uint16_t is_leaf : 1;
    std::uint16_t count : 15;

    K keys[MAX_PHYSICAL_KEYS];

    union {
      Node* children[MAX_PHYSICAL_KEYS + 1];  // used by internal nodes

      struct {  // used by leaf nodes
        V values[MAX_PHYSICAL_KEYS];
        Node* next_leaf;
        Node* prev_leaf;
      };
    };

    constexpr explicit Node(const bool leaf) noexcept : is_leaf(leaf), count(0) {
      if (leaf) {
        next_leaf = nullptr;
        prev_leaf = nullptr;
      }
    }

    ~Node() = default;
  };

  static_assert(sizeof(Node) <= NodeBytes,
                "Node size spilled out of the cache line due to padding!");

  // Read and write guards. NullLock gets none at all, not even the preemption count.
  struct NoGuard {
    explicit constexpr NoGuard(const NullLock&) noexcept {}
  };
  using ReadGuard =
      std::conditional_t<std::is_same_v<Lock, NullLock>, NoGuard, SharedPreemptGuard<Lock>>;
  using WriteGuard =
      std::conditional_t<std::is_same_v<Lock, NullLock>, NoGuard, ExclusivePreemptGuard<Lock>>;

  Node* m_root{nullptr};
  std::size_t m_size{0};
  [[no_unique_address]] mutable Lock m_lock;

  // Nodes set aside by insert() before it changes anything; splits take them from here.
  static constexpr std::size_t MAX_SPARE_NODES = 32;
  void* m_spare[MAX_SPARE_NODES]{};
  std::size_t m_spare_count{0};

  // One cache for every tree of this type, created on first use. Losing the creation race
  // just means destroying the extra cache again.
  [[nodiscard]] static memory::heap::KmemCache* node_cache() noexcept {
    static constinit std::atomic<memory::heap::KmemCache*> s_cache{nullptr};

    if (auto* cache = s_cache.load(std::memory_order_acquire)) [[likely]] {
      return cache;
    }

    const auto created = memory::heap::KmemCache::create(sizeof(Node), alignof(Node));
    if (!created) {
      return nullptr;
    }

    memory::heap::KmemCache* expected = nullptr;
    if (!s_cache.compare_exchange_strong(expected, *created, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      (*created)->destroy();
      return expected;
    }

    return *created;
  }

  [[nodiscard]] Node* take_spare(const bool is_leaf) noexcept {
    KASSERT(m_spare_count > 0);
    return new (m_spare[--m_spare_count]) Node(is_leaf);
  }

  void release_spares() noexcept {
    while (m_spare_count > 0) {
      node_cache()->free(m_spare[--m_spare_count]);
    }
  }

  // Nodes an insert of `key` can need: one per full node on the way down from the root,
  // counted upwards from the leaf for as long as the split keeps cascading, plus a new root if
  // it reaches the top.
  [[nodiscard]] std::size_t spares_needed(const K& key) const noexcept {
    if (!m_root) {
      return 1;
    }

    const Node* path[MAX_SPARE_NODES];
    std::size_t depth = 0;

    for (const Node* n = m_root;; n = n->children[search_upper_bound(n->keys, n->count, key)]) {
      if (depth == MAX_SPARE_NODES) {
        return MAX_SPARE_NODES + 1;
      }

      path[depth++] = n;
      if (n->is_leaf) {
        break;
      }
    }

    std::size_t needed = 0;
    while (needed < depth && path[depth - 1 - needed]->count == MAX_KEYS) {
      ++needed;
    }

    return needed == depth ? needed + 1 : needed;
  }

  void free_node(Node* node) noexcept {
    node->~Node();
    node_cache()->free(node);
  }

  static std::size_t search_lower_bound(const K* arr, const std::size_t len,
                                        const K& key) noexcept {
    if constexpr (MAX_PHYSICAL_KEYS <= 16) {
      [[assume(len <= MAX_PHYSICAL_KEYS)]];
      std::size_t i = 0;
      while (i < len && arr[i] < key) {
        i++;
      }

      return i;
    } else {
      std::size_t count = len;
      std::size_t first = 0;

      while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t it = first + step;

        if (arr[it] < key) {
          first = it + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }

      return first;
    }
  }

  static std::size_t search_upper_bound(const K* arr, const std::size_t len,
                                        const K& key) noexcept {
    if constexpr (MAX_PHYSICAL_KEYS <= 16) {
      [[assume(len <= MAX_PHYSICAL_KEYS)]];
      std::size_t i = 0;
      while (i < len && !(key < arr[i])) {
        i++;
      }

      return i;
    } else {
      std::size_t count = len;
      std::size_t first = 0;

      while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t it = first + step;
        if (!(key < arr[it])) {
          first = it + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }

      return first;
    }
  }

  [[nodiscard]] const Node* find_leaf(const K& key) const noexcept {
    const Node* curr = m_root;
    while (!curr->is_leaf) {
      curr = curr->children[search_upper_bound(curr->keys, curr->count, key)];
    }

    return curr;
  }

  void clear_recursive(Node* node) noexcept {
    if (!node->is_leaf) {
      for (std::size_t i = 0; i <= node->count; ++i) {
        clear_recursive(node->children[i]);
      }
    }

    free_node(node);
  }

  // Returns true if the key was new (rather than overwritten).
  bool insert_recursive(Node* node, const K& key, const V& val, K& out_split_key,
                        Node*& out_right_sibling) noexcept {
    out_right_sibling = nullptr;

    if (node->is_leaf) {
      const std::size_t idx = search_lower_bound(node->keys, node->count, key);
      if (idx < node->count && node->keys[idx] == key) {
        node->values[idx] = val;
        return false;
      }

      const std::size_t move_cnt = node->count - idx;
      if (move_cnt > 0) {
        klib::memmove(&node->keys[idx + 1], &node->keys[idx], move_cnt * sizeof(K));
        klib::memmove(&node->values[idx + 1], &node->values[idx], move_cnt * sizeof(V));
      }

      node->keys[idx] = key;
      node->values[idx] = val;
      ++node->count;
    } else {
      const std::size_t idx = search_upper_bound(node->keys, node->count, key);

      K child_split_key;
      Node* child_right_sib = nullptr;
      if (!insert_recursive(node->children[idx], key, val, child_split_key, child_right_sib)) {
        return false;
      }

      if (child_right_sib) [[unlikely]] {
        const std::size_t move_cnt = node->count - idx;
        if (move_cnt > 0) {
          klib::memmove(&node->keys[idx + 1], &node->keys[idx], move_cnt * sizeof(K));
          klib::memmove(&node->children[idx + 2], &node->children[idx + 1],
                        move_cnt * sizeof(Node*));
        }

        node->keys[idx] = child_split_key;
        node->children[idx + 1] = child_right_sib;
        ++node->count;
      }
    }

    if (node->count == MAX_PHYSICAL_KEYS) [[unlikely]] {
      out_right_sibling = take_spare(node->is_leaf);
      const std::size_t mid = MAX_PHYSICAL_KEYS / 2;

      if (node->is_leaf) {
        // Both halves keep their keys; the right one's first key goes up as a copy.
        out_right_sibling->count = MAX_PHYSICAL_KEYS - mid;
        klib::memcpy(out_right_sibling->keys, &node->keys[mid],
                     out_right_sibling->count * sizeof(K));
        klib::memcpy(out_right_sibling->values, &node->values[mid],
                     out_right_sibling->count * sizeof(V));
        out_split_key = out_right_sibling->keys[0];

        out_right_sibling->next_leaf = node->next_leaf;
        out_right_sibling->prev_leaf = node;
        if (node->next_leaf) {
          node->next_leaf->prev_leaf = out_right_sibling;
        }

        node->next_leaf = out_right_sibling;
      } else {
        // The middle key moves up and belongs to neither half.
        out_right_sibling->count = MAX_PHYSICAL_KEYS - mid - 1;
        klib::memcpy(out_right_sibling->keys, &node->keys[mid + 1],
                     out_right_sibling->count * sizeof(K));
        klib::memcpy(out_right_sibling->children, &node->children[mid + 1],
                     (out_right_sibling->count + 1) * sizeof(Node*));
        out_split_key = node->keys[mid];
      }

      node->count = mid;
    }

    return true;
  }

  // children[idx] is one key short: take the last key of its left sibling.
  static void borrow_from_left(Node* parent, const std::size_t idx) noexcept {
    Node* child = parent->children[idx];
    Node* left = parent->children[idx - 1];

    klib::memmove(&child->keys[1], &child->keys[0], child->count * sizeof(K));

    if (child->is_leaf) {
      klib::memmove(&child->values[1], &child->values[0], child->count * sizeof(V));
      child->keys[0] = left->keys[left->count - 1];
      child->values[0] = left->values[left->count - 1];
      parent->keys[idx - 1] = child->keys[0];
    } else {
      // The separator comes down, the sibling's last key goes up in its place.
      klib::memmove(&child->children[1], &child->children[0], (child->count + 1) * sizeof(Node*));
      child->keys[0] = parent->keys[idx - 1];
      child->children[0] = left->children[left->count];
      parent->keys[idx - 1] = left->keys[left->count - 1];
    }

    ++child->count;
    --left->count;
  }

  // children[idx] is one key short: take the first key of its right sibling.
  static void borrow_from_right(Node* parent, const std::size_t idx) noexcept {
    Node* child = parent->children[idx];
    Node* right = parent->children[idx + 1];

    if (child->is_leaf) {
      child->keys[child->count] = right->keys[0];
      child->values[child->count] = right->values[0];
      klib::memmove(&right->keys[0], &right->keys[1], (right->count - 1) * sizeof(K));
      klib::memmove(&right->values[0], &right->values[1], (right->count - 1) * sizeof(V));
      parent->keys[idx] = right->keys[0];
    } else {
      child->keys[child->count] = parent->keys[idx];
      child->children[child->count + 1] = right->children[0];
      parent->keys[idx] = right->keys[0];
      klib::memmove(&right->keys[0], &right->keys[1], (right->count - 1) * sizeof(K));
      klib::memmove(&right->children[0], &right->children[1], right->count * sizeof(Node*));
    }

    ++child->count;
    --right->count;
  }

  // Folds children[idx + 1] into children[idx] and drops the separator between them.
  void merge_children(Node* parent, const std::size_t idx) noexcept {
    Node* left = parent->children[idx];
    Node* right = parent->children[idx + 1];

    if (left->is_leaf) {
      klib::memcpy(&left->keys[left->count], right->keys, right->count * sizeof(K));
      klib::memcpy(&left->values[left->count], right->values, right->count * sizeof(V));
      left->count += right->count;

      left->next_leaf = right->next_leaf;
      if (right->next_leaf) {
        right->next_leaf->prev_leaf = left;
      }
    } else {
      left->keys[left->count] = parent->keys[idx];
      klib::memcpy(&left->keys[left->count + 1], right->keys, right->count * sizeof(K));
      klib::memcpy(&left->children[left->count + 1], right->children,
                   (right->count + 1) * sizeof(Node*));
      left->count += right->count + 1;
    }

    KASSERT(left->count <= MAX_KEYS);

    const std::size_t tail = parent->count - idx - 1;
    klib::memmove(&parent->keys[idx], &parent->keys[idx + 1], tail * sizeof(K));
    klib::memmove(&parent->children[idx + 1], &parent->children[idx + 2], tail * sizeof(Node*));
    --parent->count;

    free_node(right);
  }

  // Brings children[idx] of @p parent, which just dropped below MIN_KEYS, back up. Borrowing is
  // preferred: it touches two nodes and never changes the parent's size.
  void rebalance(Node* parent, const std::size_t idx) noexcept {
    Node* left = idx > 0 ? parent->children[idx - 1] : nullptr;
    Node* right = idx < parent->count ? parent->children[idx + 1] : nullptr;

    if (left && left->count > MIN_KEYS) {
      borrow_from_left(parent, idx);
    } else if (right && right->count > MIN_KEYS) {
      borrow_from_right(parent, idx);
    } else if (left) {
      merge_children(parent, idx - 1);
    } else {
      // An internal node always has at least two children, so one side exists.
      KASSERT(right != nullptr);
      merge_children(parent, idx);
    }
  }

  bool erase_recursive(Node* node, const K& key) noexcept {
    if (node->is_leaf) {
      const std::size_t idx = search_lower_bound(node->keys, node->count, key);
      if (idx == node->count || !(node->keys[idx] == key)) {
        return false;
      }

      const std::size_t move_cnt = node->count - idx - 1;
      klib::memmove(&node->keys[idx], &node->keys[idx + 1], move_cnt * sizeof(K));
      klib::memmove(&node->values[idx], &node->values[idx + 1], move_cnt * sizeof(V));
      --node->count;
      return true;
    }

    const std::size_t idx = search_upper_bound(node->keys, node->count, key);
    if (!erase_recursive(node->children[idx], key)) {
      return false;
    }

    if (node->children[idx]->count < MIN_KEYS) [[unlikely]] {
      rebalance(node, idx);
    }

    return true;
  }

 public:
  constexpr BPlusTree() noexcept = default;

  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;

  ~BPlusTree() noexcept { clear(); }

  /// @brief Number of keys in the tree.
  [[nodiscard]] std::size_t size() const noexcept {
    ReadGuard guard(m_lock);
    return m_size;
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  /// @brief Removes every key and gives the nodes back.
  void clear() noexcept {
    WriteGuard guard(m_lock);
    if (m_root) {
      clear_recursive(m_root);
      m_root = nullptr;
      m_size = 0;
    }
  }

  bool get(const K& key, V& out_val) const noexcept {
    ReadGuard guard(m_lock);
    if (!m_root) {
      return false;
    }

    const Node* leaf = find_leaf(key);
    const std::size_t idx = search_lower_bound(leaf->keys, leaf->count, key);
    if (idx < leaf->count && leaf->keys[idx] == key) [[likely]] {
      out_val = leaf->values[idx];
      return true;
    }

    return false;
  }

  [[nodiscard]] bool contains(const K& key) const noexcept {
    ReadGuard guard(m_lock);
    if (!m_root) {
      return false;
    }

    const Node* leaf = find_leaf(key);
    const std::size_t idx = search_lower_bound(leaf->keys, leaf->count, key);
    return idx < leaf->count && leaf->keys[idx] == key;
  }

  /// @brief Calls `callback(key, value)` for every key in [@p start_key, @p end_key], in order,
  ///        until it returns false.
  template <typename Func>
  void range(const K& start_key, const K& end_key, Func&& callback) const noexcept {
    ReadGuard guard(m_lock);
    if (!m_root) {
      return;
    }

    const Node* curr = m_root;
    while (!curr->is_leaf) {
      curr = curr->children[search_lower_bound(curr->keys, curr->count, start_key)];
    }

    std::size_t idx = search_lower_bound(curr->keys, curr->count, start_key);
    while (curr != nullptr) {
      while (idx < curr->count) {
        if (end_key < curr->keys[idx]) {
          return;  // Exceeded range constraint
        }

        if (!callback(curr->keys[idx], curr->values[idx])) {
          return;
        }

        ++idx;
      }

      curr = curr->next_leaf;
      idx = 0;
    }
  }

  /// @brief Inserts @p key, or overwrites its value if it is already present.
  /// @return false if a node could not be allocated, in which case the tree is unchanged.
  [[nodiscard]] bool insert(const K& key, const V& val) noexcept {
    WriteGuard guard(m_lock);

    if (!node_cache()) [[unlikely]] {
      return false;
    }

    // Allocate every node the insert may need before touching the tree, so that running out of
    // memory cannot leave it half split.
    const std::size_t needed = spares_needed(key);
    if (needed > MAX_SPARE_NODES) [[unlikely]] {
      return false;
    }

    while (m_spare_count < needed) {
      const auto mem = node_cache()->alloc();
      if (!mem) [[unlikely]] {
        release_spares();
        return false;
      }

      m_spare[m_spare_count++] = *mem;
    }

    if (!m_root) {
      m_root = take_spare(true);
    }

    K split_key;
    Node* right_node = nullptr;

    if (insert_recursive(m_root, key, val, split_key, right_node)) {
      ++m_size;
    }

    if (right_node) [[unlikely]] {
      Node* elevated_root = take_spare(false);
      elevated_root->count = 1;
      elevated_root->keys[0] = split_key;
      elevated_root->children[0] = m_root;
      elevated_root->children[1] = right_node;
      m_root = elevated_root;
    }

    release_spares();
    return true;
  }

  /// @brief Removes @p key. @return false if it was not in the tree.
  bool erase(const K& key) noexcept {
    WriteGuard guard(m_lock);
    if (!m_root || !erase_recursive(m_root, key)) {
      return false;
    }

    --m_size;

    // The root is the one node allowed to run low. Drop it once it is an empty leaf or an
    // internal node down to a single child.
    if (m_root->count == 0) {
      Node* old_root = m_root;
      m_root = m_root->is_leaf ? nullptr : m_root->children[0];
      free_node(old_root);
    }

    return true;
  }

  /// @brief Finds the largest key that is not greater than @p key.
  bool get_floor(const K& key, K& out_key, V& out_val) const noexcept {
    ReadGuard guard(m_lock);
    if (!m_root) {
      return false;
    }

    const Node* leaf = find_leaf(key);
    const std::size_t idx = search_upper_bound(leaf->keys, leaf->count, key);
    if (idx > 0) [[likely]] {
      out_key = leaf->keys[idx - 1];
      out_val = leaf->values[idx - 1];
      return true;
    }

    // Every key of this leaf is larger, and everything in the previous leaf is smaller than the
    // separator we followed, so the answer, if any, is the previous leaf's last key. Leaves other
    // than the root are never empty.
    if (const Node* prev = leaf->prev_leaf) [[unlikely]] {
      out_key = prev->keys[prev->count - 1];
      out_val = prev->values[prev->count - 1];
      return true;
    }

    return false;
  }
};
}  // namespace kernel::utils
