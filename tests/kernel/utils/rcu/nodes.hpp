#pragma once

#include <atomic>
#include <cstdint>
#include <new>

#include "ktest.hpp"
#include "memory/heap.hpp"

namespace kernel::testing::rcu_nodes {
inline void jitter(Rng& rng, const std::uint64_t most = 512) noexcept {
  for (std::uint64_t spin = rng.next() % most; spin != 0; --spin) {
    asm volatile("" ::: "memory");
  }
}

inline constexpr std::uint64_t MAGIC = 0x5CA1'AB1E'F00D'CAFEul;
struct Node {
  std::uint64_t link{0};
  std::uint64_t magic{MAGIC};
  std::uint64_t value{0};
  std::uint64_t check{~std::uint64_t{0}};
};

/// @brief Whether @p node is still what was published, and not something freed or reused.
[[nodiscard]] inline bool intact(const Node* node) noexcept {
  const std::uint64_t magic = __atomic_load_n(&node->magic, __ATOMIC_RELAXED);
  const std::uint64_t value = __atomic_load_n(&node->value, __ATOMIC_RELAXED);
  const std::uint64_t check = __atomic_load_n(&node->check, __ATOMIC_RELAXED);
  return magic == MAGIC && check == ~value;
}

/// @brief A poisoned cache of Nodes. Retired nodes go back to it through kfree(), as anything else
///        from the heap would.
class Pool {
  memory::heap::KmemCache* m_cache{nullptr};

 public:
  [[nodiscard]] bool create() noexcept {
    const auto cache = memory::heap::KmemCache::create(sizeof(Node), alignof(Node),
                                                       memory::heap::CacheFlags::Poison);
    m_cache = cache ? *cache : nullptr;
    return m_cache != nullptr;
  }

  /// @pre Everything made here is gone: rcu::barrier() has seen to it.
  void destroy() noexcept {
    if (m_cache != nullptr) {
      m_cache->destroy();
      m_cache = nullptr;
    }
  }

  [[nodiscard]] Node* make(const std::uint64_t value) noexcept {
    const auto memory = m_cache->alloc();
    if (!memory) {
      return nullptr;
    }

    return new (*memory) Node{.value = value, .check = ~value};
  }
};
}  // namespace kernel::testing::rcu_nodes
