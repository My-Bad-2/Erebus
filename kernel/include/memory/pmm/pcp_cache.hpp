/// @file pcp_cache.hpp
/// @brief Per-CPU page caches sitting in front of the buddy zones.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>

#include "page.hpp"

namespace kernel::memory::pmm {
/// @brief Orders 0 through PCP_MAX_ORDER are served from the per-CPU caches.
inline constexpr std::uint8_t PCP_MAX_ORDER = 3;

/// @brief A small per-CPU stack of free blocks of one order.
///
/// Most allocations are order 0 and are freed shortly after on the same CPU, so keeping a
/// handful of blocks per CPU avoids the zone lock entirely on the common path. The list is a
/// ring: allocation and local frees work LIFO at the tail (the most recently freed block is the
/// one most likely to still be in cache) while draining takes the oldest blocks from the head.
///
/// The zone moves blocks in and out `batch` at a time: it refills an empty list with `batch`
/// blocks and drains `batch` blocks once the list reaches `high`.
///
/// Only the owning CPU touches a list, and only with interrupts disabled.
class PcpList {
  std::uint16_t m_head{0};
  std::uint16_t m_tail{0};
  std::uint16_t m_count{0};

  std::uint16_t m_high{0};
  std::uint16_t m_batch{0};

  static constexpr std::uint16_t MAX_CAPACITY = 256;
  static constexpr std::uint16_t MASK = MAX_CAPACITY - 1;

  Page* m_pages[MAX_CAPACITY]{};

 public:
  /// @brief Sizes `high` and `batch` from the zone's free memory.
  ///
  /// Order 0 gets up to 256 cached blocks with batches of 32, and each order above that halves
  /// both. Small zones get a quarter or half of that so a few CPUs cannot hoard the whole zone.
  /// The result always satisfies `1 <= batch <= high <= MAX_CAPACITY`, which is what makes the
  /// "drain at high" rule sufficient to never overflow the ring.
  void tune(const std::uint64_t zone_free_pages, const std::uint8_t order) noexcept {
    const std::uint16_t base_high = MAX_CAPACITY >> order;
    const std::uint16_t base_batch = 32 >> order;

    std::uint16_t divisor = 1;
    if (zone_free_pages < 1024) {
      divisor = 4;
    } else if (zone_free_pages < 8192) {
      divisor = 2;
    }

    m_high = std::clamp<std::uint16_t>(base_high / divisor, 1, MAX_CAPACITY);
    m_batch = std::clamp<std::uint16_t>(base_batch / divisor, 1, m_high);
  }

  [[nodiscard]] constexpr std::uint16_t size() const noexcept { return m_count; }
  [[nodiscard]] constexpr bool empty() const noexcept { return m_count == 0; }

  [[nodiscard]] constexpr bool needs_drain() const noexcept { return m_count >= m_high; }
  [[nodiscard]] constexpr std::uint16_t batch_size() const noexcept { return m_batch; }

  /// @pre The list has room, i.e. the caller drained it once needs_drain() became true.
  [[gnu::always_inline]] void push_hot(Page* p) noexcept {
    m_pages[m_tail] = p;
    m_tail = (m_tail + 1) & MASK;
    m_count++;
  }

  /// @return The most recently pushed block, or nullptr if empty.
  [[nodiscard, gnu::always_inline]] Page* pop_hot() noexcept {
    if (m_count == 0) [[unlikely]] {
      return nullptr;
    }

    m_tail = (m_tail - 1) & MASK;
    m_count--;
    return m_pages[m_tail];
  }

  /// @return The oldest block, or nullptr if empty.
  [[nodiscard, gnu::always_inline]] Page* pop_cold() noexcept {
    if (m_count == 0) [[unlikely]] {
      return nullptr;
    }

    Page* p = m_pages[m_head];
    m_head = (m_head + 1) & MASK;
    m_count--;
    return p;
  }
};

/// @brief One CPU's cache for one zone: a list per cached order.
struct alignas(std::hardware_destructive_interference_size) PcpCache {
  std::array<PcpList, PCP_MAX_ORDER + 1> lists;
};
}  // namespace kernel::memory::pmm
