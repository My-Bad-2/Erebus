/// @file vector.hpp
/// @brief A growable array.

#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

#include "string.h"
#include "utils/assert.hpp"

namespace kernel::utils {
enum class VectorError {
  AllocationFailed,
  OutOfBounds,
  Empty,
  InvalidIterator,
};

/// @brief `std::vector` for the kernel.
///
/// Everything that allocates returns `std::expected`, and running out of memory leaves the
/// vector as it was. Elements must be nothrow movable; trivially copyable ones are moved around
/// with memcpy/memmove. Storage comes from the nothrow global allocator.
template <typename T>
class Vector {
  static_assert(std::is_nothrow_destructible_v<T>, "Types must not throw on destruction");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "Types should be noexcept move constructible");

 public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  using iterator = pointer;
  using const_iterator = const_pointer;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

 private:
  T* m_data{nullptr};
  size_type m_size{0};
  size_type m_capacity{0};

  template <typename Self>
  using propagate_const_t =
      std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const_pointer, pointer>;

  [[nodiscard]] static pointer allocate(const size_type count) noexcept {
    if (count > max_size()) [[unlikely]] {
      return nullptr;
    }

    return static_cast<pointer>(
        ::operator new(count * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow_t{}));
  }

  static void deallocate(pointer data) noexcept {
    if (data) {
      ::operator delete(data, std::align_val_t{alignof(T)});
    }
  }

  // Moves @p count elements to uninitialised memory and ends the lifetime of the originals.
  static void relocate(pointer dest, pointer src, const size_type count) noexcept {
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

  // Opens a hole of one element at @p index, with `m_size < m_capacity`. The hole is left
  // uninitialised for trivially copyable types and moved-from otherwise.
  void shift_up(const size_type index) noexcept {
    if constexpr (std::is_trivially_copyable_v<T>) {
      klib::memmove(&m_data[index + 1], &m_data[index], (m_size - index) * sizeof(T));
    } else {
      std::construct_at(&m_data[m_size], std::move(m_data[m_size - 1]));
      std::move_backward(&m_data[index], &m_data[m_size - 1], &m_data[m_size]);
    }
  }

  [[nodiscard]] constexpr size_type calculate_growth(const size_type required) const noexcept {
    const size_type grown = m_capacity == 0 ? 4 : m_capacity + (m_capacity >> 1);
    return std::max(grown, required);
  }

  // Swaps in a new buffer of @p new_capacity holding the current elements.
  [[nodiscard]] bool reallocate(const size_type new_capacity) noexcept {
    pointer new_data = allocate(new_capacity);
    if (!new_data) [[unlikely]] {
      return false;
    }

    relocate(new_data, m_data, m_size);
    deallocate(m_data);
    m_data = new_data;
    m_capacity = new_capacity;
    return true;
  }

  // emplace_back() when the buffer is full. The new element is built in the new buffer before
  // the old one goes away, so arguments that refer to elements of this vector stay valid.
  template <typename... Args>
  [[gnu::noinline]] std::expected<iterator, VectorError> emplace_back_slow(
      Args&&... args) noexcept {
    const size_type new_capacity = calculate_growth(m_size + 1);
    pointer new_data = allocate(new_capacity);
    if (!new_data) [[unlikely]] {
      return std::unexpected(VectorError::AllocationFailed);
    }

    std::construct_at(&new_data[m_size], std::forward<Args>(args)...);
    relocate(new_data, m_data, m_size);
    deallocate(m_data);

    m_data = new_data;
    m_capacity = new_capacity;
    return &m_data[m_size++];
  }

 public:
  constexpr Vector() noexcept = default;

  Vector(const Vector&) = delete;
  Vector& operator=(const Vector&) = delete;

  constexpr Vector(Vector&& other) noexcept
      : m_data(std::exchange(other.m_data, nullptr)),
        m_size(std::exchange(other.m_size, 0)),
        m_capacity(std::exchange(other.m_capacity, 0)) {}

  constexpr Vector& operator=(Vector&& other) noexcept {
    if (this != &other) {
      clear();
      deallocate(m_data);

      m_data = std::exchange(other.m_data, nullptr);
      m_size = std::exchange(other.m_size, 0);
      m_capacity = std::exchange(other.m_capacity, 0);
    }

    return *this;
  }

  ~Vector() noexcept {
    clear();
    deallocate(m_data);
  }

  template <class Self>
  [[nodiscard]] constexpr auto begin(this Self&& self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data);
  }
  template <class Self>
  [[nodiscard]] constexpr auto end(this Self&& self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data + self.m_size);
  }
  template <class Self>
  [[nodiscard]] constexpr auto rbegin(this Self&& self) noexcept {
    return std::reverse_iterator<propagate_const_t<Self>>(self.end());
  }
  template <class Self>
  [[nodiscard]] constexpr auto rend(this Self&& self) noexcept {
    return std::reverse_iterator<propagate_const_t<Self>>(self.begin());
  }

  [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return m_data; }
  [[nodiscard]] constexpr const_iterator cend() const noexcept { return m_data + m_size; }
  [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept {
    return const_reverse_iterator(cend());
  }
  [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept {
    return const_reverse_iterator(cbegin());
  }

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

    if (!reallocate(new_capacity)) [[unlikely]] {
      return std::unexpected(VectorError::AllocationFailed);
    }

    return {};
  }

  /// @brief Drops unused capacity. Keeps the old buffer if a smaller one cannot be allocated.
  bool shrink_to_fit() noexcept {
    if (m_capacity == m_size) {
      return true;
    }

    if (m_size == 0) {
      deallocate(m_data);
      m_data = nullptr;
      m_capacity = 0;
      return true;
    }

    return reallocate(m_size);
  }

  template <class Self>
  [[nodiscard]] constexpr auto&& operator[](this Self&& self, size_type index) noexcept {
    KASSERT(index < self.m_size);
    return std::forward_like<Self>(self.m_data[index]);
  }

  template <class Self>
  [[nodiscard]] constexpr auto* at(this Self&& self, size_type index) noexcept {
    if (index >= self.m_size) [[unlikely]] {
      return static_cast<propagate_const_t<Self>>(nullptr);
    }

    return self.data() + index;
  }

  template <class Self>
  [[nodiscard]] constexpr auto&& front(this Self&& self) noexcept {
    KASSERT(self.m_size != 0);
    return std::forward_like<Self>(self.m_data[0]);
  }

  template <class Self>
  [[nodiscard]] constexpr auto&& back(this Self&& self) noexcept {
    KASSERT(self.m_size != 0);
    return std::forward_like<Self>(self.m_data[self.m_size - 1]);
  }

  template <class Self>
  [[nodiscard]] constexpr auto* data(this Self&& self) noexcept {
    return static_cast<propagate_const_t<Self>>(self.m_data);
  }

  template <typename... Args>
  [[nodiscard]] std::expected<iterator, VectorError> emplace_back(Args&&... args) noexcept {
    if (m_size == m_capacity) [[unlikely]] {
      return emplace_back_slow(std::forward<Args>(args)...);
    }

    std::construct_at(&m_data[m_size], std::forward<Args>(args)...);
    return &m_data[m_size++];
  }

  [[nodiscard]] std::expected<void, VectorError> push_back(const T& value) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    return emplace_back(value).transform([](iterator) {});
  }

  [[nodiscard]] std::expected<void, VectorError> push_back(T&& value) noexcept {
    return emplace_back(std::move(value)).transform([](iterator) {});
  }

  void pop_back() noexcept {
    if (m_size > 0) [[likely]] {
      std::destroy_at(&m_data[--m_size]);
    }
  }

  [[nodiscard]] std::expected<iterator, VectorError> insert(const_iterator pos, T value) noexcept {
    const auto index = static_cast<size_type>(pos - m_data);
    if (index > m_size) [[unlikely]] {
      return std::unexpected(VectorError::InvalidIterator);
    }

    if (m_size == m_capacity) [[unlikely]] {
      if (auto res = reserve(calculate_growth(m_size + 1)); !res) {
        return std::unexpected(res.error());
      }
    }

    if (index == m_size) {
      std::construct_at(&m_data[index], std::move(value));
    } else if constexpr (std::is_trivially_copyable_v<T>) {
      shift_up(index);
      std::construct_at(&m_data[index], std::move(value));
    } else {
      shift_up(index);
      m_data[index] = std::move(value);
    }

    ++m_size;
    return m_data + index;
  }

  iterator erase(const_iterator pos) noexcept {
    const auto index = static_cast<size_type>(pos - m_data);
    if (index >= m_size) [[unlikely]] {
      return end();
    }

    if constexpr (std::is_trivially_copyable_v<T>) {
      klib::memmove(&m_data[index], &m_data[index + 1], (m_size - index - 1) * sizeof(T));
      --m_size;
    } else {
      std::move(&m_data[index + 1], &m_data[m_size], &m_data[index]);
      std::destroy_at(&m_data[--m_size]);
    }

    return m_data + index;
  }

  /// @brief Removes the element at @p pos by moving the last one into its place. O(1), but
  ///        does not keep the order.
  iterator swap_erase(const_iterator pos) noexcept {
    const auto index = static_cast<size_type>(pos - m_data);
    if (index >= m_size) [[unlikely]] {
      return end();
    }

    if (index != m_size - 1) {
      m_data[index] = std::move(m_data[m_size - 1]);
    }

    std::destroy_at(&m_data[--m_size]);
    return m_data + index;
  }

  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, T>
  [[nodiscard]] std::expected<void, VectorError> append_range(R&& range) noexcept {
    if constexpr (std::ranges::sized_range<R>) {
      const size_type required_capacity = m_size + std::ranges::size(range);
      if (required_capacity > m_capacity) {
        if (auto res = reserve(calculate_growth(required_capacity)); !res) {
          return res;
        }
      }
    }

    for (auto&& item : range) {
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
      // Geometric growth, so that growing one element at a time stays amortised O(1).
      if (count > m_capacity) {
        if (auto res = reserve(calculate_growth(count)); !res) {
          return res;
        }
      }

      std::uninitialized_value_construct_n(m_data + m_size, count - m_size);
    }

    m_size = count;
    return {};
  }

  void clear() noexcept {
    std::destroy_n(m_data, m_size);
    m_size = 0;
  }

  struct DetachedBuffer {
    T* data;
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

  template <class Self>
  [[nodiscard]] constexpr auto view(this Self&& self) noexcept {
    using ElementType =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T, T>;
    return std::span<ElementType>(self.m_data, self.m_size);
  }

  [[nodiscard]] constexpr bool operator==(const Vector& other) const noexcept {
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

  [[nodiscard]] constexpr auto operator<=>(const Vector& other) const noexcept
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(m_data, m_data + m_size, other.m_data,
                                                  other.m_data + other.m_size);
  }
};
}  // namespace kernel::utils