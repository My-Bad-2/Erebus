#pragma once

#include <array>
#include <cstdint>
#include <new>

#include "page.hpp"
#include "utils/ring_buffer.hpp"

namespace kernel::memory::pmm {
// Cache order 0, 1, 2, and 3 pages.
constexpr std::uint8_t PCP_MAX_ORDER = 3;

class PcpList {
  std::uint16_t m_head{0};
  std::uint16_t m_tail{0};
  std::uint16_t m_count{0};

  std::uint16_t m_high{0};
  std::uint16_t m_batch{0};

  static constexpr std::uint16_t MAX_CAPACITY = 256;
  static constexpr std::uint16_t MASK = MAX_CAPACITY - 1;

  Page *m_pages[MAX_CAPACITY]{};

public:
  void tune(const std::uint64_t zone_free_pages, const std::uint8_t order) noexcept {
    // Base limits: Order 0 gets 256. Order 3 gets 32.
    const std::uint16_t base_high = 256 >> order;
    const std::uint16_t base_batch = 32 >> order;

    if (zone_free_pages < 1024) {
      m_high = base_high / 4;
      m_batch = std::max<std::uint16_t>(1, base_batch / 4);
    } else if (zone_free_pages < 8192) {
      m_high = base_high / 2;
      m_batch = std::max<std::uint16_t>(1, base_batch / 2);
    } else {
      m_high = base_high;
      m_batch = base_batch;
    }

    if (m_high == 0) {
      m_high = 1;
    }

    if (m_batch == 0) {
      m_batch = 1;
    }
  }

  [[nodiscard]] constexpr std::uint16_t size() const noexcept { return m_count; }
  [[nodiscard]] constexpr bool empty() const noexcept { return m_count == 0; }

  [[nodiscard]] constexpr bool needs_drain() const noexcept { return m_count >= m_high; }
  [[nodiscard]] constexpr std::uint16_t batch_size() const noexcept { return m_batch; }

  // Push to tail
  [[gnu::always_inline]] void push_hot(Page *p) noexcept {
    if (m_count >= MAX_CAPACITY) [[unlikely]] {
      return;
    }

    m_pages[m_tail] = p;
    m_tail = (m_tail + 1) & MASK;
    m_count++;
  }

  // Pop from tail (LIFO ensures the hottest pages stay in L1 Cache)
  [[nodiscard, gnu::always_inline]] Page *pop_hot() noexcept {
    if (m_count == 0) [[unlikely]] {
      return nullptr;
    }

    m_tail = (m_tail - 1) & MASK;
    m_count--;
    return m_pages[m_tail];
  }

  // Pop from head (FIFO flushes the oldest/coldest pages to global buddy
  [[nodiscard, gnu::always_inline]] Page *pop_cold() noexcept {
    if (m_count == 0) [[unlikely]] {
      return nullptr;
    }

    Page *p = m_pages[m_head];
    m_head = (m_head + 1) & MASK;
    m_count--;
    return p;
  }
};

struct alignas(std::hardware_destructive_interference_size) PcpCache {
  std::array<PcpList, PCP_MAX_ORDER + 1> lists;
};
} // namespace kernel::memory::pmm