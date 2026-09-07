#pragma once

#include "utils/hashmap.hpp"

#include <atomic>
#include <cstdint>
#include <new>

namespace kernel::memory::vmm {
class RefCount {
  struct alignas(std::hardware_destructive_interference_size) LocalNode {
    std::atomic<std::int64_t> count{0};
  };

  utils::FastHashMap<std::uint64_t, LocalNode> m_nodes;
  std::atomic<bool> m_is_dying{false};

public:
  ~RefCount() {
    m_nodes.for_each([](std::uint64_t, const LocalNode *node) { delete node; });
  }

  void add_ref() noexcept;
  [[nodiscard]] bool drop_ref() noexcept;
};
} // namespace kernel::memory::vmm