#pragma once

#include <cstddef>
#include <cstdint>

#include "page.hpp"

namespace kernel::memory::pmm {
class alignas(32) FreeArea {
  Page *m_hot_head{nullptr};  // recently freed
  Page *m_cold_tail{nullptr}; // reclaimed / un-cached
  std::uint32_t m_nr_free{0};

public:
  constexpr FreeArea() noexcept = default;
  ~FreeArea() noexcept = default;

  FreeArea(FreeArea const &) noexcept = delete;
  FreeArea &operator=(const FreeArea &) = delete;

  [[nodiscard]] bool is_empty() const noexcept { return m_nr_free == 0; }
  [[nodiscard]] std::uint32_t count() const noexcept { return m_nr_free; }

  void push_hot(Page *page) noexcept {
    page->buddy.next = m_hot_head;
    page->buddy.prev = nullptr;

    if (m_hot_head) [[likely]] {
      m_hot_head->buddy.prev = page;
    } else [[unlikely]] {
      m_cold_tail = page;
    }

    m_hot_head = page;
    m_nr_free++;
  }

  void push_cold(Page *page) noexcept {
    page->buddy.next = nullptr;
    page->buddy.prev = m_cold_tail;

    if (m_cold_tail) [[likely]] {
      m_cold_tail->buddy.next = page;
    } else [[unlikely]] {
      m_hot_head = page;
    }

    m_cold_tail = page;
    m_nr_free++;
  }

  [[nodiscard]] Page *pop_hot() noexcept {
    Page *page = m_hot_head;

    if (page) [[likely]] {
      m_hot_head = page->buddy.next;

      if (m_hot_head) [[likely]] {
        m_hot_head->buddy.prev = nullptr;
        __builtin_prefetch(m_hot_head, 1, 3);
      } else [[unlikely]] {
        m_cold_tail = nullptr;
      }

      page->buddy.next = nullptr;
      page->buddy.prev = nullptr;
      m_nr_free--;
    }

    return page;
  }

  void remove(Page *page) noexcept {
    if (page->buddy.prev) [[likely]] {
      page->buddy.prev->buddy.next = page->buddy.next;
    } else {
      m_hot_head = page->buddy.next;
    }

    if (page->buddy.next) [[likely]] {
      page->buddy.next->buddy.prev = page->buddy.prev;
    } else {
      m_cold_tail = page->buddy.prev;
    }

    page->buddy.next = nullptr;
    page->buddy.prev = nullptr;
    m_nr_free--;
  }

  std::uint32_t steal_half(FreeArea &dest_area) noexcept {
    if (m_nr_free < 2) {
      return 0;
    }

    std::uint32_t steal_count = m_nr_free / 2;
    Page *curr = m_cold_tail;

    // Find the breaking point
    for (std::uint32_t i = 0; i < steal_count - 1; i++) {
      curr = curr->buddy.prev;
    }

    // `curr` is now the new tail of this list
    Page *stolen_head = curr->buddy.next;

    curr->buddy.next = nullptr;
    stolen_head->buddy.prev = nullptr;

    Page *stolen_tail = m_cold_tail;
    m_cold_tail = curr;
    m_nr_free -= steal_count;

    if (dest_area.m_cold_tail) {
      dest_area.m_cold_tail->buddy.next = stolen_head;
      stolen_head->buddy.prev = dest_area.m_cold_tail;
    } else {
      dest_area.m_hot_head = stolen_head;
    }

    dest_area.m_cold_tail = stolen_tail;
    dest_area.m_nr_free += steal_count;

    return steal_count;
  }
};
} // namespace kernel::memory::pmm