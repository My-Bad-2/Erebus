#pragma once

#include "string.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace kernel::utils {
enum class VectorError {
  AllocationFailed,
  OutOfBounds,
  Empty,
  InvalidIterator,
};

template <typename T> class Vector {
  static_assert(std::is_nothrow_destructible_v<T>, "Types must not throw on destruction");
  static_assert(std::is_nothrow_move_constructible_v<T>, "Types should be noexcept move constructible");

public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using reference = value_type &;
  using const_reference = const value_type &;
  using pointer = value_type *;
  using const_pointer = const value_type *;

  using iterator = pointer;
  using const_iterator = const_pointer;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

private:
  T *m_data;
  size_type m_size;
  size_type m_capacity;

  template <typename Self>
  using propagate_const_t = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_pointer, pointer>;

  void relocate_data(pointer dest, pointer src, const size_type count) noexcept {
    if constexpr (std::is_trivially_copyable_v<T>) {
      if (count > 0) {
        klib::memcpy(dest, src, count * sizeof(T));
      }
    } else {
      for (size_type i = 0; i < count; ++i) {
        std::construct_at(&dest[i], std::move(src[i]));
        std::destroy_at(&src[i]);
      }
    }
  }

  [[nodiscard]] constexpr size_type calculate_growth(const size_type required) const noexcept {
    const size_type factor = m_capacity == 0 ? 4 : m_capacity + (m_capacity >> 1);
    return std::max(factor, required);
  }

public:
  constexpr Vector() noexcept = default;

  Vector(const Vector &) = delete;
  Vector &operator=(const Vector &) = delete;

  constexpr Vector(Vector &&other) noexcept
      : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
        m_capacity(std::exchange(other.m_capacity, 0)) {}

  constexpr Vector &operator=(Vector &&other) noexcept {
    if (this != &other) {
      clear();
      if (m_data) {
        operator delete[](reinterpret_cast<std::byte *>(m_data), std::align_val_t{alignof(T)});
      }

      m_data = std::exchange(other.m_data, nullptr);
      m_size = std::exchange(other.m_size, 0);
      m_capacity = std::exchange(other.m_capacity, 0);
    }

    return *this;
  }

  ~Vector() noexcept {
    clear();
    if (m_data) {
      operator delete[](reinterpret_cast<std::byte *>(m_data), std::align_val_t{alignof(T)});
    }
  }

  template <class Self> [[nodiscard]] constexpr auto begin(this Self &&self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data);
  }
  template <class Self> [[nodiscard]] constexpr auto end(this Self &&self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data + self.m_size);
  }
  template <class Self> [[nodiscard]] constexpr auto rbegin(this Self &&self) noexcept {
    return std::reverse_iterator<propagate_const_t<Self>>(self.end());
  }
  template <class Self> [[nodiscard]] constexpr auto rend(this Self &&self) noexcept {
    return std::reverse_iterator<propagate_const_t<Self>>(self.begin());
  }

  [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return m_data; }
  [[nodiscard]] constexpr const_iterator cend() const noexcept { return m_data + m_size; }
  [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
  [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

  // --- Capacity ---
  [[nodiscard]] constexpr size_type size() const noexcept { return m_size; }
  [[nodiscard]] constexpr size_type capacity() const noexcept { return m_capacity; }
  [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }
  [[nodiscard]] static constexpr size_type max_size() noexcept {
    return std::numeric_limits<ptrdiff_t>::max() / sizeof(T);
  }

  [[nodiscard]] std::expected<void, VectorError> reserve(const size_type new_capacity) noexcept {
    if (new_capacity <= m_capacity) {
      return {};
    }

    auto *new_memory = new (std::align_val_t{alignof(T)}) std::byte[new_capacity * sizeof(T)];
    if (!new_memory) [[unlikely]] {
      return std::unexpected(VectorError::AllocationFailed);
    }

    pointer new_data = reinterpret_cast<pointer>(new_memory);
    if (m_data) {
      relocate_data(new_data, m_data, m_size);
      operator delete[](reinterpret_cast<std::byte *>(m_data), std::align_val_t{alignof(T)});
    }

    m_data = new_data;
    m_capacity = new_capacity;
    return {};
  }

  bool shrink_to_fit() noexcept {
    if (m_capacity == m_size) {
      return true;
    }

    if (m_size == 0) {
      operator delete[](reinterpret_cast<std::byte *>(m_data), std::align_val_t{alignof(T)});
      m_data = nullptr;
      m_capacity = 0;
      return true;
    }

    auto *new_memory = new (std::align_val_t{alignof(T)}) std::byte[m_size * sizeof(T)];
    if (!new_memory) [[unlikely]] {
      return false; // Just keep old capacity if allocation fails
    }

    pointer new_data = reinterpret_cast<pointer>(new_memory);
    relocate_data(new_data, m_data, m_size);
    operator delete[](reinterpret_cast<std::byte *>(m_data), std::align_val_t{alignof(T)});

    m_data = new_data;
    m_capacity = m_size;
    return true;
  }

  template <class Self> [[nodiscard]] constexpr auto &&operator[](this Self &&self, size_type index) noexcept {
    return std::forward_like<Self>(self.m_data[index]);
  }

  template <class Self> [[nodiscard]] constexpr auto *at(this Self &&self, size_type index) noexcept {
    if (index >= self.m_size) [[unlikely]] {
      return static_cast<propagate_const_t<Self>>(nullptr);
    }

    return self.data() + index;
  }

  template <class Self> [[nodiscard]] constexpr auto &&front(this Self &&self) noexcept {
    return std::forward_like<Self>(self.m_data[0]);
  }

  template <class Self> [[nodiscard]] constexpr auto &&back(this Self &&self) noexcept {
    return std::forward_like<Self>(self.m_data[self.m_size - 1]);
  }

  template <class Self> [[nodiscard]] constexpr auto *data(this Self &&self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data);
  }

  template <typename... Args> [[nodiscard]] std::expected<iterator, VectorError> emplace_back(Args &&...args) noexcept {
    if (m_size >= m_capacity) [[unlikely]] {
      if (auto res = reserve(calculate_growth(m_size + 1)); !res) {
        return std::unexpected(res.error());
      }
    }

    std::construct_at(&m_data[m_size], std::forward<Args>(args)...);
    return &m_data[m_size++];
  }

  [[nodiscard]] std::expected<void, VectorError> push_back(T value) noexcept {
    if (auto res = emplace_back(std::move(value)); !res) {
      return std::unexpected(res.error());
    }

    return {};
  }

  void pop_back() noexcept {
    if (m_size > 0) [[likely]] {
      std::destroy_at(&m_data[--m_size]);
    }
  }

  [[nodiscard]] std::expected<iterator, VectorError> insert(const_iterator pos, T value) noexcept {
    size_type index = static_cast<size_type>(pos - m_data);
    if (index > m_size) [[unlikely]] {
      return std::unexpected(VectorError::InvalidIterator);
    }

    if (m_size >= m_capacity) [[unlikely]] {
      if (auto res = reserve(calculate_growth(m_size + 1)); !res) {
        return std::unexpected(res.error());
      }
    }

    if (index < m_size) {
      std::construct_at(&m_data[m_size], std::move(m_data[m_size - 1]));
      for (size_type i = m_size - 1; i > index; --i) {
        m_data[i] = std::move(m_data[i - 1]);
      }

      m_data[index] = std::move(value);
    } else {
      std::construct_at(&m_data[index], std::move(value));
    }

    ++m_size;
    return m_data + index;
  }

  iterator erase(const_iterator pos) noexcept {
    size_type index = static_cast<size_type>(pos - m_data);
    if (index >= m_size) [[unlikely]] {
      return end();
    }

    for (size_type i = index; i < m_size - 1; ++i) {
      m_data[i] = std::move(m_data[i + 1]);
    }

    std::destroy_at(&m_data[--m_size]);
    return m_data + index;
  }

  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, T>
  [[nodiscard]] std::expected<void, VectorError> append_range(R &&range) noexcept {
    if constexpr (std::ranges::sized_range<R>) {
      const size_type required_capacity = m_size + std::ranges::size(range);
      if (required_capacity > m_capacity) {
        if (auto res = reserve(calculate_growth(required_capacity)); !res) {
          return res;
        }
      }
    }

    for (auto &&item : range) {
      if (m_size >= m_capacity) {
        if (auto res = reserve(calculate_growth(m_size + 1)); !res) {
          return res;
        }
      }

      std::construct_at(&m_data[m_size++], std::forward<decltype(item)>(item));
    }

    return {};
  }

  [[nodiscard]] std::expected<void, VectorError> resize(size_type count) noexcept {
    if (count < m_size) {
      std::destroy_n(m_data + count, m_size - count);
    } else if (count > m_size) {
      if (auto res = reserve(count); !res) {
        return res;
      }

      for (size_type i = m_size; i < count; ++i) {
        std::construct_at(&m_data[i]);
      }
    }

    m_size = count;
    return {};
  }

  void clear() noexcept {
    std::destroy_n(m_data, m_size);
    m_size = 0;
  }

  struct DetachedBuffer {
    T *data;
    size_type size;
    size_type capacity;
  };

  [[nodiscard]] DetachedBuffer release() noexcept {
    DetachedBuffer buffer{m_data, m_size, m_capacity};
    m_data = nullptr;
    m_size = 0;
    m_capacity = 0;
    return buffer;
  }

  template <class Self> [[nodiscard]] constexpr auto view(this Self &&self) noexcept {
    using ElementType = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T, T>;
    return std::span<ElementType>(self.m_data, self.m_size);
  }

  [[nodiscard]] constexpr bool operator==(const Vector &other) const noexcept {
    if (m_size != other.m_size) {
      return false;
    }

    for (size_type i = 0; i < m_size; ++i) {
      if (m_data[i] != other.m_data[i]) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] constexpr auto operator<=>(const Vector &other) const noexcept
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(m_data, m_data + m_size, other.m_data, other.m_data + other.m_size);
  }
};
} // namespace kernel::utils