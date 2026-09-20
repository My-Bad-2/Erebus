#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace kernel::utils {
template <typename T, typename Tag = void> class SList;
template <typename T, typename Tag, bool Const> class SListIterator;

template <typename T, typename Tag = void> class SListNode {
  SListNode *m_next{nullptr};
  friend class SList<T, Tag>;
  friend class SListIterator<T, Tag, true>;
  friend class SListIterator<T, Tag, false>;

public:
  constexpr SListNode() noexcept = default;
  constexpr ~SListNode() noexcept = default;

  SListNode(const SListNode &) = delete;
  SListNode &operator=(const SListNode &) = delete;
  SListNode(SListNode &&) = delete;
  SListNode &operator=(SListNode &&) = delete;

  [[nodiscard]] constexpr T *as_item() noexcept { return static_cast<T *>(this); }
  [[nodiscard]] constexpr const T *as_item() const noexcept { return static_cast<const T *>(this); }
  [[nodiscard]] constexpr bool is_linked() const noexcept { return m_next != nullptr; }
};

template <typename T, typename Tag, bool Const> class SListIterator {
  using node_type = std::conditional_t<Const, const SListNode<T, Tag>, SListNode<T, Tag>>;
  node_type *m_current;

  friend class SList<T, Tag>;
  template <typename, typename, bool> friend class SListIterator;

  explicit constexpr SListIterator(node_type *node) noexcept : m_current(node) {}

public:
  using iterator_category = std::forward_iterator_tag;
  using iterator_concept = std::forward_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<Const, const T *, T *>;
  using reference = std::conditional_t<Const, const T &, T &>;

  constexpr SListIterator() noexcept : m_current(nullptr) {}

  constexpr SListIterator(const SListIterator &) noexcept = default;
  constexpr SListIterator &operator=(const SListIterator &) noexcept = default;
  constexpr SListIterator(SListIterator &&) noexcept = default;
  constexpr SListIterator &operator=(SListIterator &&) noexcept = default;

  constexpr SListIterator(const SListIterator<T, Tag, false> &other) noexcept
    requires Const
      : m_current(other.m_current) {}

  [[nodiscard]] constexpr reference operator*() const noexcept {
    [[assume(m_current != nullptr)]];
    return *m_current->as_item();
  }

  [[nodiscard]] constexpr pointer operator->() const noexcept {
    [[assume(m_current != nullptr)]];
    return m_current->as_item();
  }

  constexpr SListIterator &operator++() noexcept {
    [[assume(m_current != nullptr)]];
    m_current = m_current->m_next;
    return *this;
  }

  constexpr SListIterator operator++(int) noexcept {
    SListIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  [[nodiscard]] friend constexpr bool operator==(const SListIterator &, const SListIterator &) noexcept = default;
};

template <typename T, typename Tag> class SList {
  static_assert(std::derived_from<T, SListNode<T, Tag>>, "SList type T must derive from SListNode<T>");
  using node_type = SListNode<T, Tag>;
  node_type m_head{};

public:
  using value_type = T;
  using size_type = std::size_t;
  using iterator = SListIterator<T, Tag, false>;
  using const_iterator = SListIterator<T, Tag, true>;

  constexpr SList() noexcept = default;

  SList(const SList &) = delete;
  SList &operator=(const SList &) = delete;

  constexpr SList(SList &&other) noexcept { m_head.m_next = std::exchange(other.m_head.m_next, nullptr); }

  constexpr SList &operator=(SList &&other) noexcept {
    if (this != &other) {
      m_head.m_next = std::exchange(other.m_head.m_next, nullptr);
    }

    return *this;
  }

  template <class Self> [[nodiscard]] constexpr auto begin(this Self &&self) noexcept {
    using Iter = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator>;
    using NodePtr = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const node_type *, node_type *>;
    return Iter(static_cast<NodePtr>(self.m_head.m_next));
  }

  template <class Self> [[nodiscard]] constexpr auto end(this Self &&) noexcept {
    using Iter = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator>;
    return Iter(nullptr);
  }

  template <class Self> [[nodiscard]] constexpr auto before_begin(this Self &&self) noexcept {
    using Iter = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_iterator, iterator>;
    using NodePtr = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const node_type *, node_type *>;
    return Iter(static_cast<NodePtr>(&self.m_head));
  }

  template <class Self> [[nodiscard]] constexpr decltype(auto) front(this Self &&self) noexcept {
    [[assume(self.m_head.m_next != nullptr)]];
    using NodePtr = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const node_type *, node_type *>;
    return *static_cast<NodePtr>(self.m_head.m_next)->as_item();
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return m_head.m_next == nullptr; }

  constexpr void push_front(T &item) noexcept {
    node_type *node = static_cast<node_type *>(&item);
    if (node->m_next != nullptr) {
      std::unreachable();
    }

    node->m_next = m_head.m_next;
    m_head.m_next = node;
  }

  constexpr T *pop_front() noexcept {
    [[assume(m_head.m_next != nullptr)]];

    node_type *front_node = m_head.m_next;
    m_head.m_next = front_node->m_next;

    front_node->m_next = nullptr;
    return front_node->as_item();
  }

  constexpr iterator insert_after(const_iterator pos, T &item) noexcept {
    node_type *pos_node = const_cast<node_type *>(pos.m_current);
    node_type *new_node = static_cast<node_type *>(&item);

    [[assume(new_node->m_next == nullptr)]];

    new_node->m_next = pos_node->m_next;
    pos_node->m_next = new_node;

    return iterator(new_node);
  }

  constexpr T *erase_after(const_iterator pos) noexcept {
    node_type *pos_node = const_cast<node_type *>(pos.m_current);
    [[assume(pos_node->m_next != nullptr)]];

    node_type *target_node = pos_node->m_next;
    pos_node->m_next = target_node->m_next;

    target_node->m_next = nullptr; // Unlink
    return target_node->as_item();
  }

  constexpr bool remove(T &item) noexcept {
    node_type *target = static_cast<node_type *>(&item);
    node_type *curr = &m_head;

    while (curr->m_next != nullptr) {
      if (curr->m_next == target) {
        curr->m_next = target->m_next;
        target->m_next = nullptr; // Unlink
        return true;
      }

      curr = curr->m_next;
    }

    return false;
  }

  constexpr void reverse() noexcept {
    node_type *prev = nullptr;
    node_type *curr = m_head.m_next;
    node_type *next = nullptr;

    while (curr != nullptr) {
      next = curr->m_next;
      curr->m_next = prev;
      prev = curr;
      curr = next;
    }

    m_head.m_next = prev;
  }

  constexpr void clear() noexcept { m_head.m_next = nullptr; }
};
} // namespace kernel::utils