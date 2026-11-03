/// @file free_area.hpp
/// @brief Free list of one buddy order.

#pragma once

#include <cstdint>

#include "page.hpp"

namespace kernel::memory::pmm {
/// @brief Doubly linked list of free blocks of a single order.
///
/// The list has a hot end and a cold end. Blocks freed at runtime are pushed hot because their
/// cache lines are likely still resident; memory handed over at boot is pushed cold so that
/// recently used blocks are always preferred. Allocation only ever pops from the hot end.
///
/// @note Not thread-safe: the owning MobilityZone serialises access with its lock.
class alignas(32) FreeArea {
  Page* m_hot_head{nullptr};
  Page* m_cold_tail{nullptr};
  std::uint32_t m_nr_free{0};

 public:
  constexpr FreeArea() noexcept = default;
  ~FreeArea() noexcept = default;

  FreeArea(const FreeArea&) noexcept = delete;
  FreeArea& operator=(const FreeArea&) = delete;

  [[nodiscard]] bool is_empty() const noexcept { return m_nr_free == 0; }
  [[nodiscard]] std::uint32_t count() const noexcept { return m_nr_free; }

  void push_hot(Page* page) noexcept {
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

  void push_cold(Page* page) noexcept {
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

  /// @return The hottest block, or nullptr if the list is empty.
  [[nodiscard]] Page* pop_hot() noexcept {
    Page* page = m_hot_head;

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

  /// @pre @p page is linked into *this* list.
  void remove(Page* page) noexcept {
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
};
}  // namespace kernel::memory::pmm
