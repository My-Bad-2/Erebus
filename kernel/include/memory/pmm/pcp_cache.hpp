#pragma once

#include <array>
#include <cstdint>
#include <new>

#include "page.hpp"
#include "utils/ring_buffer.hpp"

namespace kernel::memory::pmm {
// Cache order 0, 1, 2, and 3 pages.
constexpr std::uint8_t PCP_MAX_ORDER = 3;
constexpr std::uint32_t PCP_CAPACITY = 256;

struct PcpList {
  std::uint16_t high{PCP_CAPACITY};
  std::uint16_t batch{32};

  utils::RingDeque<Page *, PCP_CAPACITY> deque;

  void tune(const std::uint64_t zone_free_pages) noexcept {
    if (zone_free_pages < 1024) {
      high = 32;
      batch = 8;
    } else if (zone_free_pages < 8192) {
      high = 128;
      batch = 16;
    } else {
      high = 256;
      batch = 32;
    }
  }

  [[gnu::always_inline]] void push_hot(Page *p) noexcept { static_cast<void>(deque.emplace_back(p)); }

  [[nodiscard, gnu::always_inline]] Page *pop_hot() noexcept { return *deque.pop_back(); }
  [[nodiscard, gnu::always_inline]] Page *pop_cold() noexcept { return *deque.pop_front(); }
};

struct alignas(std::hardware_destructive_interference_size) PcpCache {
  std::array<PcpList, PCP_MAX_ORDER + 1> lists;
};
} // namespace kernel::memory::pmm