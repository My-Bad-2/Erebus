/// @file hashmap.hpp
/// @brief Sharded open-addressing hash map from 64-bit keys to pointers.

#pragma once

#include <string.h>

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

#include "locks/guards.hpp"
#include "memory/heap.hpp"

namespace kernel::utils {
template <typename T>
concept KernelKey = std::unsigned_integral<T> && sizeof(T) == 8;

namespace detail::swiss {
// Control bytes: a live slot holds the top 7 bits of its key's hash, the other two states have
// the top bit set so that one mask finds every free slot.
inline constexpr std::uint8_t CTRL_EMPTY = 0x80;
inline constexpr std::uint8_t CTRL_DELETED = 0xC0;
inline constexpr std::uint32_t GROUP_SIZE = 8;

inline constexpr std::uint64_t LSB = 0x0101010101010101ul;
inline constexpr std::uint64_t MSB = 0x8080808080808080ul;

/// @brief One 64x64->128 bit multiply, folded. Every output bit depends on every input bit, so
///        keys that only differ in a few low bits (page numbers, aligned addresses) spread out.
[[nodiscard, gnu::always_inline]] constexpr std::uint64_t hash(const std::uint64_t key) noexcept {
  const __uint128_t product =
      static_cast<__uint128_t>(key ^ 0x9E3779B97F4A7C15ul) * 0xBF58476D1CE4E5B9ul;
  return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);
}

/// @brief High bit of every byte of @p group equal to @p ctrl.
///
/// The usual "has zero byte" trick. It can flag a byte right above a real match as well, which
/// for signatures only costs an extra key comparison. For CTRL_EMPTY it is exact: a false hit
/// would need a 0x81 control byte, which never exists.
[[nodiscard, gnu::always_inline]] constexpr std::uint64_t match(const std::uint64_t group,
                                                                const std::uint8_t ctrl) noexcept {
  const std::uint64_t x = group ^ (LSB * ctrl);
  return (x - LSB) & ~x & MSB;
}

/// @brief High bit of every free (empty or deleted) byte of @p group.
[[nodiscard, gnu::always_inline]] constexpr std::uint64_t match_free(
    const std::uint64_t group) noexcept {
  return group & MSB;
}

[[nodiscard, gnu::always_inline]] inline std::uint64_t load_group(
    const std::uint8_t* ctrl) noexcept {
  std::uint64_t group;
  __builtin_memcpy(&group, ctrl, sizeof(group));
  return group;
}
}  // namespace detail::swiss

/// @brief Concurrent map from 64-bit keys to `V*`, split into independently locked shards.
///
/// Each shard is a Swiss-table style open-addressing table: one control byte per slot holding 7
/// bits of the key's hash, scanned eight at a time with plain 64-bit arithmetic, so most misses
/// and hits are decided without looking at a key. Probing walks whole groups of eight.
///
/// The hash is split three ways: the top 7 bits are the control byte, the bits below pick the
/// shard, and the low bits pick the slot, so none of them is correlated with another.
///
/// The map stores pointers and never owns what they point to. lookup() returns the pointer as
/// it was under the shard's lock; keeping the object alive afterwards is the caller's business.
template <KernelKey K, typename V, std::size_t NumShards = 64>
class FastHashMap {
  static_assert(std::has_single_bit(NumShards), "NumShards must be a power of 2");
  static constexpr std::uint32_t SHARD_BITS = std::countr_zero(NumShards);
  static_assert(SHARD_BITS <= 16, "too many shards");

  static constexpr std::uint32_t GROUP = detail::swiss::GROUP_SIZE;
  // Smallest table that holds a whole group.
  static constexpr std::uint32_t MIN_CAPACITY = 16;

  struct Entry {
    K key;
    V* value;
  };

  // One allocation: this header, then the entries, then one control byte per entry.
  struct Table {
    std::uint32_t capacity;
    std::uint32_t mask;
    Entry* entries;
    std::uint8_t* ctrl;
  };

  struct alignas(std::hardware_destructive_interference_size) Shard {
    mutable RWLock lock;
    std::uint32_t size{0};
    std::uint32_t tombstones{0};  // deleted slots; probes run through them like live ones
    Table* table{nullptr};
  };

  Shard m_shards[NumShards];

  struct Hashed {
    std::uint64_t hash;
    std::uint8_t sig;
    Shard* shard;
  };

  [[nodiscard]] Hashed locate(const K key) noexcept {
    const std::uint64_t hash = detail::swiss::hash(key);
    const auto shard = static_cast<std::size_t>((hash >> (57 - SHARD_BITS)) & (NumShards - 1));
    return {hash, static_cast<std::uint8_t>(hash >> 57), &m_shards[shard]};
  }

  [[nodiscard]] static std::uint32_t first_group(const Table* tbl,
                                                 const std::uint64_t hash) noexcept {
    return static_cast<std::uint32_t>(hash) & tbl->mask & ~(GROUP - 1);
  }

  [[nodiscard]] static std::uint32_t next_group(const Table* tbl,
                                                const std::uint32_t idx) noexcept {
    return (idx + GROUP) & tbl->mask;
  }

  // Tables come straight from kmalloc so that running out of memory is an error the caller
  // sees, not a panic in operator new.
  [[nodiscard]] static Table* allocate_table(const std::uint32_t capacity) noexcept {
    const std::size_t bytes = sizeof(Table) + sizeof(Entry) * capacity + capacity;
    auto* tbl = static_cast<Table*>(memory::heap::kmalloc(bytes, alignof(Table)));
    if (!tbl) {
      return nullptr;
    }

    tbl->capacity = capacity;
    tbl->mask = capacity - 1;
    tbl->entries = reinterpret_cast<Entry*>(tbl + 1);
    tbl->ctrl = reinterpret_cast<std::uint8_t*>(tbl->entries + capacity);
    klib::memset(tbl->ctrl, detail::swiss::CTRL_EMPTY, capacity);
    return tbl;
  }

  // Finds the slot of @p key, or returns `capacity` if it is not in the table.
  [[nodiscard]] static std::uint32_t find_slot(const Table* tbl, const K key,
                                               const Hashed& h) noexcept {
    using namespace detail::swiss;

    for (std::uint32_t idx = first_group(tbl, h.hash);; idx = next_group(tbl, idx)) {
      const std::uint64_t group = load_group(&tbl->ctrl[idx]);

      for (std::uint64_t hits = match(group, h.sig); hits != 0; hits &= hits - 1) {
        const std::uint32_t slot = idx + (std::countr_zero(hits) >> 3);
        if (tbl->entries[slot].key == key) [[likely]] {
          return slot;
        }
      }

      // Inserts fill the first free slot on the probe path, so a key never lives past a group
      // that still has an empty slot.
      if (match(group, CTRL_EMPTY) != 0) {
        return tbl->capacity;
      }
    }
  }

  // Inserts a key known to be absent into a table known to have room.
  static void insert_unique(Table* tbl, const K key, V* value, const std::uint8_t sig,
                            const std::uint64_t hash) noexcept {
    using namespace detail::swiss;

    for (std::uint32_t idx = first_group(tbl, hash);; idx = next_group(tbl, idx)) {
      const std::uint64_t free = match_free(load_group(&tbl->ctrl[idx]));
      if (free != 0) {
        const std::uint32_t slot = idx + (std::countr_zero(free) >> 3);
        tbl->entries[slot] = {key, value};
        tbl->ctrl[slot] = sig;
        return;
      }
    }
  }

  [[gnu::cold]] static bool rehash(Shard& shard) noexcept {
    Table* old_tbl = shard.table;

    // Grow if the table is genuinely filling up; if it is mostly tombstones, rebuilding at the
    // same size is enough to get the empty slots back.
    const bool grow = shard.size * 2 >= old_tbl->capacity;
    if (grow && old_tbl->capacity > std::numeric_limits<std::uint32_t>::max() / 2) [[unlikely]] {
      return false;
    }

    Table* new_tbl = allocate_table(grow ? old_tbl->capacity * 2 : old_tbl->capacity);
    if (!new_tbl) [[unlikely]] {
      return false;
    }

    for (std::uint32_t i = 0; i < old_tbl->capacity; ++i) {
      if ((old_tbl->ctrl[i] & detail::swiss::CTRL_EMPTY) == 0) {
        const Entry& e = old_tbl->entries[i];
        insert_unique(new_tbl, e.key, e.value, old_tbl->ctrl[i], detail::swiss::hash(e.key));
      }
    }

    memory::heap::kfree(old_tbl);
    shard.table = new_tbl;
    shard.tombstones = 0;
    return true;
  }

 public:
  // Shards get their table on first insert, so an empty map costs nothing and constructing one
  // cannot fail.
  explicit FastHashMap() noexcept = default;

  FastHashMap(const FastHashMap&) = delete;
  FastHashMap& operator=(const FastHashMap&) = delete;

  ~FastHashMap() noexcept {
    // The values are not ours to free.
    for (Shard& shard : m_shards) {
      memory::heap::kfree(shard.table);
    }
  }

  [[nodiscard]] V* lookup(const K key) noexcept {
    const Hashed h = locate(key);
    SharedPreemptGuard guard(h.shard->lock);

    const Table* tbl = h.shard->table;
    if (!tbl) {
      return nullptr;
    }

    const std::uint32_t slot = find_slot(tbl, key, h);
    return slot != tbl->capacity ? tbl->entries[slot].value : nullptr;
  }

  /// @brief Adds @p key. @return false if it is already present or memory ran out.
  bool insert(const K key, V* value) noexcept {
    using namespace detail::swiss;

    const Hashed h = locate(key);
    Shard& shard = *h.shard;
    ExclusivePreemptGuard guard(shard.lock);

    if (!shard.table) [[unlikely]] {
      shard.table = allocate_table(MIN_CAPACITY);
      if (!shard.table) {
        return false;
      }
    }

    // Lookups stop at the first group with an empty slot, so tombstones count against the load
    // factor too; otherwise erase-heavy use could leave no empty slot and a miss would never end.
    // If growing fails we may still use the table as long as one empty slot is left over.
    if ((static_cast<std::uint64_t>(shard.size) + shard.tombstones) * 8 >=
        static_cast<std::uint64_t>(shard.table->capacity) * 7) {
      if (!rehash(shard) && shard.size + shard.tombstones + 1 >= shard.table->capacity) {
        return false;
      }
    }

    Table* tbl = shard.table;
    std::uint32_t target = tbl->capacity;

    // One pass: look for a duplicate and remember the first free slot on the way.
    for (std::uint32_t idx = first_group(tbl, h.hash);; idx = next_group(tbl, idx)) {
      const std::uint64_t group = load_group(&tbl->ctrl[idx]);

      for (std::uint64_t hits = match(group, h.sig); hits != 0; hits &= hits - 1) {
        if (tbl->entries[idx + (std::countr_zero(hits) >> 3)].key == key) {
          return false;
        }
      }

      if (target == tbl->capacity) {
        if (const std::uint64_t free = match_free(group); free != 0) {
          target = idx + (std::countr_zero(free) >> 3);
        }
      }

      if (match(group, CTRL_EMPTY) != 0) {
        break;
      }
    }

    if (tbl->ctrl[target] == CTRL_DELETED) {
      --shard.tombstones;
    }

    tbl->entries[target] = {key, value};
    tbl->ctrl[target] = h.sig;
    ++shard.size;
    return true;
  }

  /// @brief Removes @p key. @return Its value, or null if it was not present.
  [[nodiscard]] V* erase(const K key) noexcept {
    using namespace detail::swiss;

    const Hashed h = locate(key);
    Shard& shard = *h.shard;
    ExclusivePreemptGuard guard(shard.lock);

    Table* tbl = shard.table;
    if (!tbl) {
      return nullptr;
    }

    const std::uint32_t slot = find_slot(tbl, key, h);
    if (slot == tbl->capacity) {
      return nullptr;
    }

    // A group that still has an empty slot never made a probe go past it, so the slot can be
    // freed for real. Otherwise later keys may sit behind it and it has to stay a tombstone.
    const std::uint32_t group_start = slot & ~(GROUP - 1);
    if (match(load_group(&tbl->ctrl[group_start]), CTRL_EMPTY) != 0) {
      tbl->ctrl[slot] = CTRL_EMPTY;
    } else {
      tbl->ctrl[slot] = CTRL_DELETED;
      ++shard.tombstones;
    }

    --shard.size;
    return tbl->entries[slot].value;
  }

  /// @brief Number of keys. Shards are read one at a time, so it is a snapshot at best.
  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t total = 0;
    for (const Shard& shard : m_shards) {
      SharedPreemptGuard guard(shard.lock);
      total += shard.size;
    }

    return total;
  }

  /// @brief Calls `callback(key, value)` for every entry, one shard at a time under its lock.
  template <typename Func>
  void for_each(Func&& callback) noexcept {
    for (Shard& shard : m_shards) {
      SharedPreemptGuard guard(shard.lock);

      const Table* tbl = shard.table;
      if (!tbl) {
        continue;
      }

      for (std::uint32_t i = 0; i < tbl->capacity; ++i) {
        if ((tbl->ctrl[i] & detail::swiss::CTRL_EMPTY) == 0) {
          callback(tbl->entries[i].key, tbl->entries[i].value);
        }
      }
    }
  }
};
}  // namespace kernel::utils
