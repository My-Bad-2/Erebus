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
  asm volatile("crc32q %1, %0" : "+r"(hash) : "rm"(key));
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
};

template <KernelKey K, typename V, std::size_t NumShards = 64> class FastHashMap {
  static_assert(std::has_single_bit(NumShards), "NumShards must be a power of 2");

  struct alignas(std::hardware_destructive_interference_size) Shard {
    mutable RWLock lock;
    MapTable<K, V> *table{nullptr};
    std::size_t size{0};
  };

  Shard m_shards[NumShards];

  [[nodiscard]] MapTable<K, V> *allocate_table(std::uint32_t capacity) noexcept {
    auto *tbl = new MapTable<K, V>();

    tbl->capacity = capacity;
    tbl->mask = capacity - 1;

    tbl->ctrl = new std::uint8_t[capacity + 8];
    tbl->keys = new K[capacity];
    tbl->values = new V *[capacity];

    klib::memset(tbl->ctrl, CTRL_EMPTY, capacity + 8);
    return tbl;
  }

  static void free_table(MapTable<K, V> *tbl) noexcept {
    if (!tbl) {
      return;
    }

    delete[] tbl->values;
    delete[] tbl->keys;
    delete[] tbl->ctrl;
    delete tbl;
  }

  void insert_no_lock(MapTable<K, V> *tbl, K key, V *val, std::uint8_t sig) noexcept {
    std::uint32_t idx = (hash_key(key) & tbl->mask) & ~7u;

    while (true) {
      const auto ctrl_word = *reinterpret_cast<std::uint64_t *>(&tbl->ctrl[idx]);
      const std::uint64_t usable = swar_match_usable(ctrl_word);

      if (usable) {
        // Bit pos it offset by max 7 slots. target max is (capacity - 8) + 7 = capacity - 1
        std::uint32_t target = idx + (std::countr_zero(usable) >> 3);
        tbl->keys[target] = key;
        tbl->values[target] = val;
        tbl->ctrl[target] = sig;
        return;
      }

      idx = (idx + 8) & tbl->mask;
    }
  }

  [[gnu::cold]] bool rehash(Shard &shard) noexcept {
    MapTable<K, V> *old_tbl = shard.table;
    MapTable<K, V> *new_tbl = allocate_table(old_tbl->capacity * 2);

    for (std::uint32_t i = 0; i < old_tbl->capacity; ++i) {
      if ((old_tbl->ctrl[i] & CTRL_EMPTY) == 0) {
        insert_no_lock(new_tbl, old_tbl->keys[i], old_tbl->values[i], old_tbl->ctrl[i]);
      }
    }

    free_table(new_tbl);
    shard.table = new_tbl;
    return true;
  }

public:
  explicit FastHashMap() noexcept {
    for (std::size_t i = 0; i < NumShards; ++i) {
      // 16 is the minimum capacity required for safe SWAR reads
      m_shards[i].table = allocate_table(16);
    }
  }

  ~FastHashMap() noexcept {
    for (std::size_t i = 0; i < NumShards; ++i) {
      // We do not assume ownership of V* objects, that is left to the caller
      free_table(m_shards[i].table);
    }
  }

  [[nodiscard]] V *lookup(K key) noexcept {
    const std::uint64_t hash = hash_key(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

    const std::size_t shard_idx = hash & (NumShards - 1);
    Shard &shard = m_shards[shard_idx];

    SharedPreemptGuard guard(shard.lock);
    MapTable<K, V> *tbl = shard.table;

    [[assume(tbl->capacity >= 16 && (tbl->capacity & tbl->mask) == 0)]];

    std::uint32_t idx = (hash & tbl->mask) & ~7u;

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
    }
  }

  bool insert(K key, V *value) noexcept {
    const std::uint64_t hash = hash_key(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

    const std::size_t shard_idx = hash & (NumShards - 1);
    Shard &shard = m_shards[shard_idx];

    ExclusivePreemptGuard guard(shard.lock);

    if (shard.size * 8 > shard.table->capacity * 7) {
      if (!rehash(shard) && shard.size >= shard.table->capacity * 7 / 8) {
        return false;
      }
    }

    MapTable<K, V> *tbl = shard.table;
    std::uint32_t idx = (hash & tbl->mask) & ~7u;
    std::uint32_t target_slot = std::numeric_limits<std::uint32_t>::max();

    while (true) {
      const std::uint64_t ctrl_word = *reinterpret_cast<std::uint64_t *>(&tbl->ctrl[idx]);
      std::uint64_t match_mask = swar_match_sig(ctrl_word, sig);

      while (match_mask) {
        const int bit_pos = std::countr_zero(match_mask);
        std::uint32_t chk_idx = idx + (bit_pos >> 3);
        if (tbl->keys[chk_idx] == key) {
          // Duplicate
          return false;
        }

        match_mask &= ~(0xFFul << bit_pos);
      }

      if (target_slot == std::numeric_limits<std::uint32_t>::max()) {
        const std::uint64_t usable = swar_match_usable(ctrl_word);
        if (usable) {
          target_slot = idx + (std::countr_zero(usable) >> 3);
        }
      }

      if (swar_match_sig(ctrl_word, CTRL_EMPTY)) {
        break;
      }

      idx = (idx + 8) & tbl->mask;
    }

    if (target_slot != std::numeric_limits<std::uint32_t>::max()) {
      tbl->keys[target_slot] = key;
      tbl->values[target_slot] = value;
      tbl->ctrl[target_slot] = sig;

      ++shard.size;
      return true;
    }

    return false;
  }

  [[nodiscard]] V *erase(K key) noexcept {
    const std::uint64_t hash = hash_key(key);
    const std::uint8_t sig = static_cast<std::uint8_t>(hash >> 57) & 0x7F;

    const std::size_t shard_idx = hash & (NumShards - 1);
    Shard &shard = m_shards[shard_idx];

    ExclusivePreemptGuard guard(shard.lock);
    MapTable<K, V> *tbl = shard.table;

    std::uint32_t idx = (hash & tbl->mask) & ~7u;

    while (true) {
      const std::uint64_t ctrl_word = *reinterpret_cast<std::uint64_t *>(&tbl->ctrl[idx]);
      std::uint64_t match_mask = swar_match_sig(ctrl_word, sig);

      while (match_mask) {
        const int bit_pos = std::countr_zero(match_mask);
        std::uint32_t target_idx = idx + (bit_pos >> 3);

        if (tbl->keys[target_idx] == key) {
          V *val = tbl->values[target_idx];
          tbl->ctrl[target_idx] = CTRL_DELETED;
          --shard.size;
          return val;
        }

        match_mask &= ~(0xFFul << bit_pos);
      }

      if (swar_match_sig(ctrl_word, CTRL_EMPTY)) {
        return nullptr;
      }

      idx = (idx + 8) & tbl->mask;
    }
  }

  template <typename Func> void for_each(Func &&callback) noexcept {
    for (std::size_t i = 0; i < NumShards; ++i) {
      Shard &shard = m_shards[i];
      SharedPreemptGuard guard(shard.lock);

      MapTable<K, V> *tbl = shard.table;
      for (std::uint32_t j = 0; j < tbl->capacity; ++j) {
        if ((tbl->ctrl[j] & CTRL_EMPTY) == 0 && tbl->ctrl[j] != CTRL_DELETED) {
          callback(tbl->keys[j], tbl->values[j]);
        }
      }
    }
  }
};
} // namespace kernel::utils