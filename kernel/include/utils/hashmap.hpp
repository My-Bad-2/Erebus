#pragma once

#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <new>

#include <string.h>

#include "locks/guards.hpp"
#include "memory/heap.hpp"

#include <immintrin.h>

namespace kernel::utils {
template <typename T>
concept KernelKey = std::unsigned_integral<T> && sizeof(T) == 8;

constexpr std::uint8_t CTRL_EMPTY = 0x80;
constexpr std::uint8_t CTRL_DELETED = 0xC0;

[[nodiscard]] inline std::uint64_t hash_key(const std::uint64_t key) noexcept {
  std::uint64_t hash = 0x9e3779b97f4a7c15ul;
  hash = _mm_crc32_u64(hash, key);
  return hash ^ (hash >> 32);
}

[[nodiscard]] constexpr std::uint64_t replicate_byte(const std::uint8_t b) noexcept {
  return static_cast<std::uint64_t>(b) * 0x0101010101010101ul;
}

[[nodiscard]] inline std::uint64_t swar_match_sig(const std::uint64_t ctrl_word, const std::uint8_t sig) noexcept {
  const std::uint64_t match = ctrl_word ^ replicate_byte(sig);
  return (match - 0x0101010101010101ul) & ~match & 0x8080808080808080ul;
}

[[nodiscard]] inline std::uint64_t swar_match_usable(const std::uint64_t ctrl_word) noexcept {
  return ctrl_word & 0x8080808080808080ul;
}

template <KernelKey K, typename V> struct alignas(std::hardware_destructive_interference_size) MapTable {
  std::uint32_t capacity;
  std::uint32_t mask;
  std::uint8_t *ctrl;
  K *keys;
  V **values;

  QSpinlock *group_locks; // 1 lock per 64 slots
};

template <KernelKey K, typename V> class FastHashMap {
  alignas(std::hardware_destructive_interference_size) RWLock m_global_lock;

  MapTable<K, V> *m_table{nullptr};
  memory::heap::KmemCache *m_value_cache{nullptr};
  std::atomic<std::size_t> m_size{0};

  [[nodiscard]] MapTable<K, V> *allocate_table(std::uint32_t capacity) noexcept {
    auto *tbl = new MapTable<K, V>();
    if (!tbl) [[unlikely]] {
      return nullptr;
    }

    tbl->capacity = capacity;
    tbl->mask = capacity - 1;

    tbl->ctrl = new std::uint8_t[capacity + 8];
    if (tbl->ctrl) {
      klib::memset(tbl->ctrl, CTRL_EMPTY, capacity + 8);
    }

    tbl->keys = new (std::nothrow) K[capacity];
    tbl->values = new (std::nothrow) V *[capacity];

    const std::uint32_t num_groups = capacity / 64;
    tbl->group_locks = new (std::nothrow) QSpinlock[num_groups];

    if (!tbl->ctrl || !tbl->keys || !tbl->values || !tbl->group_locks) {
      delete[] tbl->group_locks;
      delete[] tbl->values;
      delete[] tbl->keys;
      delete[] tbl->ctrl;
      delete tbl;
      return nullptr;
    }

    return tbl;
  }

  static void free_table(MapTable<K, V> *tbl) noexcept {
    if (!tbl) {
      return;
    }

    delete[] tbl->group_locks;
    delete[] tbl->values;
    delete[] tbl->keys;
    delete[] tbl->ctrl;
    delete tbl;
  }

  void insert_no_lock(MapTable<K, V> *tbl, K key, V *val, std::uint8_t sig) noexcept {
    std::uint32_t idx = (hash_key(key) & tbl->mask) & ~7u;
    while (true) {
      const auto ctrl_word = *reinterpret_cast<const std::uint64_t *>(&tbl->ctrl[idx]);
      const std::uint64_t usable = swar_match_usable(ctrl_word);

      if (usable) {
        std::uint32_t target = idx + (std::countr_zero(usable) >> 3);
        tbl->keys[target] = key;
        tbl->values[target] = val;
        tbl->ctrl[target] = sig;
        return;
      }

      idx = (idx + 8) & tbl->mask;
    }
  }

  [[gnu::cold]] void rehash() noexcept {
    ExclusivePreemptGuard guard(m_global_lock);

    if (m_size.load(std::memory_order_relaxed) * 8 <= m_table->capacity * 7) {
      return;
    }

    MapTable<K, V> *new_tbl = allocate_table(m_table->capacity * 2);
    if (!new_tbl) [[unlikely]] {
      return;
    }

    for (std::uint32_t i = 0; i < m_table->capacity; ++i) {
      if ((m_table->ctrl[i] & CTRL_EMPTY) == 0) {
        insert_no_lock(new_tbl, m_table->keys[i], m_table->values[i], m_table->ctrl[i]);
      }
    }

    free_table(new_tbl);
    m_table = new_tbl;
  }

public:
  explicit FastHashMap() noexcept { m_table = allocate_table(128); }

  ~FastHashMap() noexcept {
    if (m_table) {
      for (std::uint32_t i = 0; i < m_table->capacity; ++i) {
        if ((m_table->ctrl[i] & CTRL_EMPTY) == 0) {
          delete m_table->values[i];
        }
      }

      free_table(m_table);
    }
  }

  [[nodiscard]] V *lookup(K key) noexcept {
    const std::uint64_t hash = hash_key(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

    SharedPreemptGuard guard(m_global_lock);
    MapTable<K, V> *tbl = m_table;

    [[assume(tbl->capacity >= 64 && (tbl->capacity & tbl->mask) == 0)]];

    std::uint32_t idx = (hash & tbl->mask) & ~7u;
    std::uint32_t curr_group = idx / 64;

    while (true) {
      bool cross_group = false;
      {
        NakedGuard group_guard(tbl->group_locks[curr_group]);

        while (true) {
          const std::uint64_t ctrl_word = *reinterpret_cast<const std::uint64_t *>(&tbl->ctrl[idx]);
          std::uint64_t match_mask = swar_match_sig(ctrl_word, sig);

          while (match_mask) {
            const int bit_pos = std::countr_zero(match_mask);
            std::uint32_t target_idx = idx + (bit_pos >> 3);

            if (tbl->keys[target_idx] == key) [[likely]] {
              return tbl->values[target_idx];
            }

            match_mask &= ~(0xFFul << bit_pos);
          }

          if (swar_match_sig(ctrl_word, CTRL_EMPTY)) {
            return nullptr;
          }

          idx = (idx + 8) & tbl->mask;
          if ((idx / 64) != curr_group) {
            curr_group = idx / 64;
            cross_group = true;
            break;
          }
        }
      }

      if (!cross_group) {
        break;
      }
    }

    return nullptr;
  }

  bool insert(K key, V *value) noexcept {
    if (m_size.load(std::memory_order_relaxed) * 8 > m_table->capacity * 7) {
      rehash();
    }

    const std::uint64_t hash = hash_key_x86(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

  retry:
    SharedPreemptGuard global_guard(m_global_lock);
    MapTable<K, V> *tbl = m_table;

    std::uint32_t idx = (hash & tbl->mask) & ~7U;
    std::uint32_t curr_group = idx / 64;

    std::uint32_t target_slot = 0xFFFFFFFF;
    std::uint32_t target_group = 0;

    while (true) {
      bool cross_group = false;
      {
        NakedGuard group_guard(tbl->group_locks[curr_group]);

        while (true) {
          std::uint64_t ctrl_word = *reinterpret_cast<const std::uint64_t *>(&tbl->ctrl[idx]);
          std::uint64_t match_mask = swar_match_sig(ctrl_word, sig);

          while (match_mask) {
            const int bit_pos = std::countr_zero(match_mask);
            std::uint32_t chk_idx = idx + (bit_pos >> 3);
            if (tbl->keys[chk_idx] == key) {
              return false;
            }

            match_mask &= ~(0xFFul << bit_pos);
          }

          if (target_slot == 0xFFFFFFFF) {
            const std::uint64_t usable = swar_match_usable(ctrl_word);
            if (usable) {
              target_slot = idx + (std::countr_zero(usable) >> 3);
              target_group = curr_group;
            }
          }

          if (swar_match_sig(ctrl_word, CTRL_EMPTY)) {
            goto perform_write;
          }

          idx = (idx + 8) & tbl->mask;
          if ((idx / 64) != curr_group) {
            curr_group = idx / 64;
            cross_group = true;
            break;
          }
        }
      }

      if (!cross_group) {
        break;
      }
    }

  perform_write: {
    NakedGuard write_guard(tbl->group_locks[target_group]);

    if ((tbl->ctrl[target_slot] & 0x80) != 0) [[likely]] {
      tbl->keys[target_slot] = key;
      tbl->values[target_slot] = value;
      tbl->ctrl[target_slot] = sig;

      m_size.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

    goto retry;
  }

  bool erase(K key) noexcept {
    const std::uint64_t hash = hash_key_x86(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

    SharedPreemptGuard global_guard(m_global_lock);
    MapTable<K, V> *tbl = m_table;

    std::uint32_t idx = (hash & tbl->mask) & ~7u;
    std::uint32_t curr_group = idx / 64;

    while (true) {
      bool cross_group = false;
      {
        NakedGuard group_guard(tbl->group_locks[curr_group]);

        while (true) {
          const std::uint64_t ctrl_word = *reinterpret_cast<const std::uint64_t *>(&tbl->ctrl[idx]);
          std::uint64_t match_mask = swar_match_sig(ctrl_word, sig);

          while (match_mask) {
            const int bit_pos = std::countr_zero(match_mask);
            std::uint32_t target_idx = idx + (bit_pos >> 3);

            if (tbl->keys[target_idx] == key) [[likely]] {
              const V *val = tbl->values[target_idx];

              tbl->ctrl[target_idx] = CTRL_DELETED;
              m_size.fetch_sub(1, std::memory_order_relaxed);

              delete val;
              return true;
            }

            match_mask &= ~(0xFFul << bit_pos);
          }

          if (swar_match_sig(ctrl_word, CTRL_EMPTY)) {
            return false;
          }

          idx = (idx + 8) & tbl->mask;
          if ((idx / 64) != curr_group) {
            curr_group = idx / 64;
            cross_group = true;
            break;
          }
        }
      }

      if (!cross_group) {
        break;
      }
    }

    return false;
  }
};
} // namespace kernel::utils