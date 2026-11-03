/// @file ref_counter.hpp
/// @brief Reference count for VM objects.

#pragma once

#include <atomic>
#include <cstdint>

#include "utils/logger.hpp"

namespace kernel::memory::vmm {
/// @brief An atomic reference count that starts out owned by its creator.
///
/// VM object references change when mappings are created, split, merged, forked or torn down,
/// never on the fault path, so a single counter is plenty. (A per-node split counter cannot tell
/// "zero" from "momentarily summed to zero" without a much heavier protocol, which is exactly the
/// kind of bug a reference count must not have.)
class RefCount {
  std::atomic<std::uint64_t> m_count;

 public:
  constexpr explicit RefCount(const std::uint64_t initial = 1) noexcept : m_count{initial} {}

  RefCount(const RefCount&) = delete;
  RefCount& operator=(const RefCount&) = delete;

  /// @pre The caller already holds a reference.
  void add_ref() noexcept { m_count.fetch_add(1, std::memory_order_relaxed); }

  /// @return true if this dropped the last reference; the caller then destroys the object.
  [[nodiscard]] bool drop_ref() noexcept {
    const std::uint64_t prev = m_count.fetch_sub(1, std::memory_order_release);
    if (prev == 1) {
      // Make every other owner's writes visible before the object is torn down.
      std::atomic_thread_fence(std::memory_order_acquire);
      return true;
    }

    if (prev == 0) [[unlikely]] {
      utils::logger::fatal("RefCount: dropped a reference that was never taken\n");
    }

    return false;
  }

  [[nodiscard]] std::uint64_t count() const noexcept {
    return m_count.load(std::memory_order_relaxed);
  }
};
}  // namespace kernel::memory::vmm
