#include "../../../../include/memory/vmm/vma/ref_counter.hpp"

namespace kernel::memory::vmm {
void RefCount::add_ref() noexcept {
  const auto node_id = hw::percpu::numa_node();
  LocalNode *local = m_nodes.lookup(node_id);

  if (!local) [[unlikely]] {
    auto new_node = new LocalNode();

    if (m_nodes.insert(node_id, new_node)) {
      local = new_node;
    } else {
      // Another thread won the insertion race.
      delete new_node;
      local = m_nodes.lookup(node_id);
    }
  }

  local->count.fetch_add(1, std::memory_order_relaxed);
}

bool RefCount::drop_ref() noexcept {
  const auto node_id = hw::percpu::numa_node();
  LocalNode *local = m_nodes.lookup(node_id);
  if (!local) {
    return false;
  }

  const auto prev = local->count.fetch_sub(1, std::memory_order_release);
  if (prev > 1 && !m_is_dying.load(std::memory_order_relaxed)) [[unlikely]] {
    return false;
  }

  std::atomic_thread_fence(std::memory_order_acquire);

  std::uint64_t total = 0;
  m_nodes.for_each(
      [&total](std::uint64_t, const LocalNode *node) -> void { total += node->count.load(std::memory_order_relaxed); });

  if (total <= 0) {
    m_is_dying.store(true, std::memory_order_release);
    return true;
  }

  return false;
}
} // namespace kernel::memory::vmm