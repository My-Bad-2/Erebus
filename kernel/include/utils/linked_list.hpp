/// @file linked_list.hpp
/// @brief Intrusive singly linked list.
///
/// Items derive from SListNode and carry their own link, so the list never allocates. A list is
/// terminated by a sentinel node shared by every list of the same type rather than by null,
/// which leaves null free to mean "not on any list": `is_linked()` is exact, and linking a node
/// twice is caught by an assertion in debug builds.

#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

#include "utils/assert.hpp"

namespace kernel::utils {
template <typename T, typename Tag = void>
class SList;
template <typename T, typename Tag, bool Const>
class SListIterator;

/// @brief The link an item needs to be on an SList. @p Tag tells apart several links in one item.
template <typename T, typename Tag = void>
class SListNode {
  SListNode* m_next{nullptr};
  friend class SList<T, Tag>;
  friend class SListIterator<T, Tag, true>;
  friend class SListIterator<T, Tag, false>;

 public:
  constexpr SListNode() noexcept = default;
  constexpr ~SListNode() noexcept = default;

  SListNode(const SListNode&) = delete;
  SListNode& operator=(const SListNode&) = delete;
  SListNode(SListNode&&) = delete;
  SListNode& operator=(SListNode&&) = delete;

  [[nodiscard]] constexpr T* as_item() noexcept { return static_cast<T*>(this); }
  [[nodiscard]] constexpr const T* as_item() const noexcept { return static_cast<const T*>(this); }

  /// @brief Whether the item is on a list right now.
  [[nodiscard]] constexpr bool is_linked() const noexcept { return m_next != nullptr; }
};

namespace detail {
// End of every SList<T, Tag>. Never dereferenced as an item.
template <typename T, typename Tag>
inline constinit SListNode<T, Tag> slist_end{};
}  // namespace detail

template <typename T, typename Tag, bool Const>
class SListIterator {
  using node_type = std::conditional_t<Const, const SListNode<T, Tag>, SListNode<T, Tag>>;
  node_type* m_current;

  friend class SList<T, Tag>;
  template <typename, typename, bool>
  friend class SListIterator;

  explicit constexpr SListIterator(node_type* node) noexcept : m_current(node) {}

 public:
  using iterator_category = std::forward_iterator_tag;
  using iterator_concept = std::forward_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<Const, const T*, T*>;
  using reference = std::conditional_t<Const, const T&, T&>;

  constexpr SListIterator() noexcept : m_current(nullptr) {}

  constexpr SListIterator(const SListIterator&) noexcept = default;
  constexpr SListIterator& operator=(const SListIterator&) noexcept = default;
  constexpr SListIterator(SListIterator&&) noexcept = default;
  constexpr SListIterator& operator=(SListIterator&&) noexcept = default;

  constexpr SListIterator(const SListIterator<T, Tag, false>& other) noexcept
    requires Const
      : m_current(other.m_current) {}

  [[nodiscard]] constexpr reference operator*() const noexcept { return *m_current->as_item(); }

  [[nodiscard]] constexpr pointer operator->() const noexcept { return m_current->as_item(); }

  constexpr SListIterator& operator++() noexcept {
    m_current = m_current->m_next;
    return *this;
  }

  constexpr SListIterator operator++(int) noexcept {
    SListIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  [[nodiscard]] friend constexpr bool operator==(const SListIterator&,
                                                 const SListIterator&) noexcept = default;
};

/// @brief Intrusive singly linked list of @p T, which must derive from `SListNode<T, Tag>`.
///
/// The list does not own its items. push_front(), pop_front() and the `_after` operations are
/// O(1); remove() has to find the predecessor and is O(n).
///
/// A list may be declared while @p T is still incomplete, as long as nothing is done with it
/// until @p T is known. That is how a per-CPU run queue holds lists of threads without everything
/// that includes the per-CPU area having to know what a thread is.
template <typename T, typename Tag>
class SList {
  using node_type = SListNode<T, Tag>;

  [[nodiscard]] static constexpr node_type* sentinel() noexcept {
    return &detail::slist_end<T, Tag>;
  }

  [[nodiscard]] static constexpr node_type* node_of(T& item) noexcept {
    static_assert(std::derived_from<T, node_type>, "SList type T must derive from SListNode<T>");
    return static_cast<node_type*>(&item);
  }

  node_type m_head{};

  template <class Self>
  using iterator_for =
      std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                         SListIterator<T, Tag, true>, SListIterator<T, Tag, false>>;

 public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = SListIterator<T, Tag, false>;
  using const_iterator = SListIterator<T, Tag, true>;

  constexpr SList() noexcept { m_head.m_next = sentinel(); }

  SList(const SList&) = delete;
  SList& operator=(const SList&) = delete;

  constexpr SList(SList&& other) noexcept {
    m_head.m_next = std::exchange(other.m_head.m_next, sentinel());
  }

  /// @brief Takes over @p other's items. Items already on this list are unlinked first.
  constexpr SList& operator=(SList&& other) noexcept {
    if (this != &other) {
      clear();
      m_head.m_next = std::exchange(other.m_head.m_next, sentinel());
    }

    return *this;
  }

  template <class Self>
  [[nodiscard]] constexpr auto begin(this Self&& self) noexcept {
    return iterator_for<Self>(self.m_head.m_next);
  }

  template <class Self>
  [[nodiscard]] constexpr auto end(this Self&&) noexcept {
    return iterator_for<Self>(sentinel());
  }

  template <class Self>
  [[nodiscard]] constexpr auto before_begin(this Self&& self) noexcept {
    return iterator_for<Self>(&self.m_head);
  }

  template <class Self>
  [[nodiscard]] constexpr decltype(auto) front(this Self&& self) noexcept {
    KASSERT(!self.empty());
    return *iterator_for<Self>(self.m_head.m_next);
  }

  /// @brief An iterator at @p item, which must be on this list. O(1).
  [[nodiscard]] constexpr iterator iterator_to(T& item) noexcept { return iterator(node_of(item)); }

  [[nodiscard]] constexpr bool empty() const noexcept { return m_head.m_next == sentinel(); }

  constexpr void push_front(T& item) noexcept {
    node_type* node = node_of(item);
    KASSERT(!node->is_linked());

    node->m_next = m_head.m_next;
    m_head.m_next = node;
  }

  /// @pre The list is not empty.
  constexpr T* pop_front() noexcept {
    KASSERT(!empty());

    node_type* front_node = m_head.m_next;
    m_head.m_next = front_node->m_next;

    front_node->m_next = nullptr;
    return front_node->as_item();
  }

  /// @brief Links @p item right after @p pos, which may be before_begin().
  constexpr iterator insert_after(const_iterator pos, T& item) noexcept {
    node_type* pos_node = const_cast<node_type*>(pos.m_current);
    node_type* new_node = node_of(item);
    KASSERT(!new_node->is_linked() && pos_node != sentinel());

    new_node->m_next = pos_node->m_next;
    pos_node->m_next = new_node;

    return iterator(new_node);
  }

  /// @brief Unlinks the item after @p pos and returns it.
  /// @pre There is an item after @p pos.
  constexpr T* erase_after(const_iterator pos) noexcept {
    node_type* pos_node = const_cast<node_type*>(pos.m_current);
    KASSERT(pos_node != sentinel() && pos_node->m_next != sentinel());

    node_type* target_node = pos_node->m_next;
    pos_node->m_next = target_node->m_next;

    target_node->m_next = nullptr;
    return target_node->as_item();
  }

  /// @brief Unlinks @p item if it is on this list.
  /// @return false if it is not.
  constexpr bool remove(T& item) noexcept {
    node_type* target = node_of(item);
    if (!target->is_linked()) {
      return false;
    }

    for (node_type* curr = &m_head; curr->m_next != sentinel(); curr = curr->m_next) {
      if (curr->m_next == target) {
        curr->m_next = target->m_next;
        target->m_next = nullptr;
        return true;
      }
    }

    return false;
  }

  constexpr void reverse() noexcept {
    node_type* prev = sentinel();
    node_type* curr = m_head.m_next;

    while (curr != sentinel()) {
      node_type* next = curr->m_next;
      curr->m_next = prev;
      prev = curr;
      curr = next;
    }

    m_head.m_next = prev;
  }

  /// @brief Unlinks every item. O(n), since each item has to learn it is free again.
  constexpr void clear() noexcept {
    node_type* curr = m_head.m_next;
    while (curr != sentinel()) {
      curr = std::exchange(curr->m_next, nullptr);
    }

    m_head.m_next = sentinel();
  }
};
}  // namespace kernel::utils
