#pragma once

#include "logger.hpp"
#include "memory/heap.hpp"
#include "utils/lock.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string.h>
#include <string_view>
#include <type_traits>

namespace kernel::utils {
template <typename T>
concept BTreeData = requires(T a, T b) {
  { a < b } -> std::same_as<bool>;
  { a == b } -> std::same_as<bool>;
} && std::is_trivially_copyable_v<T>;

template <BTreeData K, BTreeData V, size_t NodeBytes = std::hardware_destructive_interference_size> class BPlusTree {
  static consteval std::size_t calc_max_keys() {
    for (std::size_t n = 64; n >= 3; --n) {
      std::size_t offset = sizeof(uint16_t); // Header

      offset = maths::align_up(offset, alignof(K));
      offset += n * sizeof(K);

      const std::size_t children_size = (n + 1) * sizeof(void *);
      const std::size_t leaf_data_size = n * sizeof(V) + 2 * sizeof(void *); // values + next_leaf + prev_leaf

      const std::size_t union_size = (children_size > leaf_data_size) ? children_size : leaf_data_size;
      const std::size_t union_align = (alignof(V) > alignof(void *)) ? alignof(V) : alignof(void *);

      offset = maths::align_up(offset, union_align);

      if (offset + union_size <= NodeBytes) {
        return n;
      }
    }

    return 0;
  }

  static constexpr size_t MAX_PHYSICAL_KEYS = calc_max_keys();
  static_assert(MAX_PHYSICAL_KEYS >= 3, "NodeBytes too small to satisfy minimum branching factor.");

  struct alignas(std::hardware_destructive_interference_size) Node {
    std::uint16_t is_leaf : 1;
    std::uint16_t count : 15;

    K keys[MAX_PHYSICAL_KEYS];

    union {
      Node *children[MAX_PHYSICAL_KEYS + 1]; // used by internal nodes

      struct { // used by leaf nodes
        V values[MAX_PHYSICAL_KEYS];
        Node *next_leaf;
        Node *prev_leaf;
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

  static_assert(sizeof(Node) <= NodeBytes, "Node size spilled out of the cache line due to padding!");

  memory::heap::KmemCache *m_node_cache;
  Node *m_root;
  mutable RWLock m_lock;

  Node *allocate_node(const bool is_leaf) noexcept {
    auto mem = m_node_cache->alloc();
    if (!mem) {
      return nullptr;
    }

    return new (*mem) Node(is_leaf);
  }

  void free_node(Node *node) noexcept {
    if (!node) {
      return;
    }

    node->~Node();
    m_node_cache->free(node);
  }

  static std::size_t search_lower_bound(const K *arr, const std::size_t len, const K &key) noexcept {
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
        std::size_t it = first + step;

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

  static std::size_t search_upper_bound(const K *arr, std::size_t len, const K &key) noexcept {
    if constexpr (MAX_PHYSICAL_KEYS <= 16) {
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
        std::size_t it = first + step;
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

  void clear_recursive(Node *node) noexcept {
    if (!node) {
      return;
    }

    if (!node->is_leaf) {
      for (std::size_t i = 0; i <= node->count; ++i) {
        clear_recursive(node->children[i]);
      }
    }

    free_node(node);
  }

  void insert_recursive(Node *node, const K &key, const V &val, K &out_split_key, Node *&out_right_sibling) noexcept {
    out_right_sibling = nullptr;

    if (node->is_leaf) {
      const std::size_t idx = search_lower_bound(node->keys, node->count, key);
      if (idx < node->count && node->keys[idx] == key) [[likely]] {
        node->values[idx] = val;
        return;
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
      Node *child_right_sib = nullptr;
      insert_recursive(node->children[idx], key, val, child_split_key, child_right_sib);

      if (child_right_sib) [[unlikely]] {
        const std::size_t move_cnt = node->count - idx;
        if (move_cnt > 0) {
          klib::memmove(&node->keys[idx + 1], &node->keys[idx], move_cnt * sizeof(K));
          klib::memmove(&node->children[idx + 2], &node->children[idx + 1], move_cnt * sizeof(Node *));
        }

        node->keys[idx] = child_split_key;
        node->children[idx + 1] = child_right_sib;
        ++node->count;
      }
    }

    if (node->count == MAX_PHYSICAL_KEYS) [[unlikely]] {
      out_right_sibling = allocate_node(node->is_leaf);
      const std::size_t mid = MAX_PHYSICAL_KEYS / 2;

      out_right_sibling->count = MAX_PHYSICAL_KEYS - mid - 1;
      node->count = mid;

      klib::memcpy(out_right_sibling->keys, &node->keys[mid + 1], out_right_sibling->count * sizeof(K));

      if (node->is_leaf) {
        ++out_right_sibling->count;
        klib::memcpy(&out_right_sibling->keys[0], &node->keys[mid], out_right_sibling->count * sizeof(K));
        klib::memcpy(&out_right_sibling->values[0], &node->values[mid], out_right_sibling->count * sizeof(V));
        out_split_key = out_right_sibling->keys[0];

        out_right_sibling->next_leaf = node->next_leaf;
        out_right_sibling->prev_leaf = node;
        if (node->next_leaf) {
          node->next_leaf->prev_leaf = out_right_sibling;
        }

        node->next_leaf = out_right_sibling;
      } else {
        klib::memcpy(out_right_sibling->children, &node->children[mid + 1],
                     (out_right_sibling->count + 1) * sizeof(Node *));
        out_split_key = node->keys[mid];
      }
    }
  }

  bool erase_recursive(Node *node, const K &key) {
    if (node->is_leaf) {
      const std::size_t idx = search_lower_bound(node->keys, node->count, key);

      if (idx < node->count && node->keys[idx] == key) [[likely]] {
        const std::size_t move_cnt = node->count - idx - 1;
        if (move_cnt > 0) {
          klib::memmove(&node->keys[idx], &node->keys[idx + 1], move_cnt * sizeof(K));
          klib::memmove(&node->values[idx], &node->values[idx + 1], move_cnt * sizeof(V));
        }

        --node->count;
      }

      return node->count == 0;
    }

    const std::size_t idx = search_upper_bound(node->keys, node->count, key);

    if (erase_recursive(node->children[idx], key)) [[unlikely]] {
      const Node *child = node->children[idx];
      if (child->is_leaf) {
        if (child->prev_leaf) {
          child->prev_leaf->next_leaf = child->next_leaf;
        }

        if (child->next_leaf) {
          child->next_leaf->prev_leaf = child->prev_leaf;
        }
      }

      free_node(node->children[idx]);

      if (node->count == 0) {
        return true;
      }

      const std::size_t key_idx = (idx > 0) ? (idx - 1) : 0;
      const std::size_t move_keys = node->count - key_idx - 1;
      const std::size_t move_children = node->count - idx;

      if (move_keys > 0) {
        klib::memmove(&node->keys[key_idx], &node->keys[key_idx + 1], move_keys * sizeof(K));
      }

      if (move_children > 0) {
        klib::memmove(&node->children[idx], &node->children[idx + 1], move_children * sizeof(Node *));
      }

      --node->count;
    }

    return false;
  }

public:
  BPlusTree() {
    const auto res = memory::heap::KmemCache::create("bplus_node", sizeof(Node), alignof(Node));
    if (!res) {
      logger::fatal("Unable to create memory block allocator.");
    }

    m_node_cache = *res;
    m_root = allocate_node(true);
  }

  ~BPlusTree() {
    clear_recursive(m_root);
    m_node_cache->destroy();
  }

  bool get(const K &key, V &out_val) const noexcept {
    SharedPreemptGuard guard(m_lock);
    Node *curr = m_root;

    while (!curr->is_leaf) {
      std::size_t idx = search_upper_bound(curr->keys, curr->count, key);
      Node *next = curr->children[idx];
      __builtin_prefetch(next, 0, 3);
      curr = next;
    }

    std::size_t idx = search_lower_bound(curr->keys, curr->count, key);
    if (idx < curr->count && curr->keys[idx] == key) [[likely]] {
      out_val = curr->values[idx];
      return true;
    }

    return false;
  }

  bool contains(const K &key) const noexcept {
    V dummy;
    return get(key, dummy);
  }

  template <typename Func> void range(const K &start_key, const K &end_key, Func &&callback) const noexcept {
    SharedPreemptGuard guard(m_lock);

    Node *curr = m_root;

    while (!curr->is_leaf) {
      std::size_t idx = search_lower_bound(curr->keys, curr->count, start_key);
      curr = curr->children[idx];
    }

    std::size_t idx = search_lower_bound(curr->keys, curr->count, start_key);
    while (curr != nullptr) {
      while (idx < curr->count) {
        if (end_key < curr->keys[idx]) {
          return; // Exceeded range constraint
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

  void insert(const K &key, const V &val) noexcept {
    ExclusivePreemptGuard guard(m_lock);

    K split_key;
    Node *right_node = nullptr;

    insert_recursive(m_root, key, val, split_key, right_node);
    if (right_node) [[unlikely]] {
      Node *elevated_root = allocate_node(false);
      elevated_root->count = 1;
      elevated_root->keys[0] = split_key;
      elevated_root->children[0] = m_root;
      elevated_root->children[1] = right_node;
      m_root = elevated_root;
    }
  }

  void erase(const K &key) noexcept {
    ExclusivePreemptGuard guard(m_lock);

    if (m_root->count == 0 && m_root->is_leaf) {
      return;
    }

    erase_recursive(m_root, key);
    while (!m_root->is_leaf && m_root->count == 0) {
      Node *old_root = m_root;
      m_root = m_root->children[0];
      free_node(old_root);
    }
  }

  bool get_floor(const K &key, K &out_key, V &out_val) const noexcept {
    SharedPreemptGuard guard(m_lock);
    Node *curr = m_root;

    while (!curr->is_leaf) {
      const std::size_t idx = search_upper_bound(curr->keys, curr->count, key);
      Node *next = curr->children[idx];
      __builtin_prefetch(next, 0, 3);
      curr = next;
    }

    const std::size_t idx = search_upper_bound(curr->keys, curr->count, key);
    if (idx > 0) [[likely]] {
      out_key = curr->keys[idx - 1];
      out_val = curr->values[idx - 1];
      return true;
    }

    // Stale key
    if (curr->prev_leaf) [[unlikely]] {
      out_key = curr->prev_leaf->keys[curr->prev_leaf->count - 1];
      out_val = curr->prev_leaf->values[curr->prev_leaf->count - 1];
      return true;
    }

    return false;
  }
};
} // namespace kernel::utils