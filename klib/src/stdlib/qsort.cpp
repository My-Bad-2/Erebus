#include <bit>
#include <cstddef>
#include <cstdint>

#include "../include/string.hpp"
#include "stdlib.h"

namespace klib {
namespace {
using Compare = int (*)(const void*, const void*);

// Partitions this short are left for the insertion sort pass at the end.
constexpr std::size_t INSERTION_THRESHOLD = 16;

[[gnu::always_inline]] inline void swap_bytes(void* a, void* b, std::size_t size) noexcept {
  using string::unaligned_load;
  using string::unaligned_store;

  auto* p1 = static_cast<char*>(a);
  auto* p2 = static_cast<char*>(b);

  for (; size >= 8; size -= 8, p1 += 8, p2 += 8) {
    const auto tmp = unaligned_load<std::uint64_t>(p1);
    unaligned_store(p1, unaligned_load<std::uint64_t>(p2));
    unaligned_store(p2, tmp);
  }

  if (size >= 4) {
    const auto tmp = unaligned_load<std::uint32_t>(p1);
    unaligned_store(p1, unaligned_load<std::uint32_t>(p2));
    unaligned_store(p2, tmp);
    size -= 4;
    p1 += 4;
    p2 += 4;
  }

  for (; size > 0; --size, ++p1, ++p2) {
    const char tmp = *p1;
    *p1 = *p2;
    *p2 = tmp;
  }
}

class Sorter {
  char* m_base;
  std::size_t m_size;
  Compare m_compare;

  [[nodiscard]] char* at(const std::size_t i) const noexcept { return m_base + i * m_size; }

  [[nodiscard]] bool less(const std::size_t i, const std::size_t j) const noexcept {
    return m_compare(at(i), at(j)) < 0;
  }

  void swap(const std::size_t i, const std::size_t j) const noexcept {
    swap_bytes(at(i), at(j), m_size);
  }

  void sift_down(std::size_t root, const std::size_t first, const std::size_t end) const noexcept {
    while (true) {
      std::size_t child = 2 * root + 1;
      if (child >= end) {
        return;
      }

      if (child + 1 < end && less(first + child, first + child + 1)) {
        ++child;
      }

      if (!less(first + root, first + child)) {
        return;
      }

      swap(first + root, first + child);
      root = child;
    }
  }

  // Sorts [first, first + count) in O(n log n), whatever the input.
  void heap_sort(const std::size_t first, const std::size_t count) const noexcept {
    for (std::size_t i = count / 2; i-- > 0;) {
      sift_down(i, first, count);
    }

    for (std::size_t end = count - 1; end > 0; --end) {
      swap(first, first + end);
      sift_down(0, first, end);
    }
  }

  // Hoare partition of [first, first + count) around the median of three. Returns where the
  // pivot ended up; everything before it compares <= and everything after >=.
  [[nodiscard]] std::size_t partition(const std::size_t first,
                                      const std::size_t count) const noexcept {
    const std::size_t mid = first + count / 2;
    const std::size_t last = first + count - 1;

    if (less(mid, first)) swap(mid, first);
    if (less(last, first)) swap(last, first);
    if (less(last, mid)) swap(last, mid);
    swap(mid, first);

    // Both scans stop on elements equal to the pivot, which keeps runs of equal keys balanced.
    std::size_t left = first + 1;
    std::size_t right = last;
    while (true) {
      while (left <= right && less(left, first)) ++left;
      while (left <= right && less(first, right)) --right;
      if (left >= right) {
        break;
      }

      swap(left++, right--);
    }

    swap(first, right);
    return right;
  }

 public:
  Sorter(void* base, const std::size_t size, const Compare compare) noexcept
      : m_base(static_cast<char*>(base)), m_size(size), m_compare(compare) {}

  // Quicksort down to short partitions, falling back to heap sort once the recursion runs
  // deeper than it should, which bounds the worst case at O(n log n). Recursing into the
  // smaller side keeps the stack at O(log n).
  void introsort(std::size_t first, std::size_t count, std::size_t depth_limit) const noexcept {
    while (count > INSERTION_THRESHOLD) {
      if (depth_limit-- == 0) [[unlikely]] {
        heap_sort(first, count);
        return;
      }

      const std::size_t pivot = partition(first, count);
      const std::size_t left_count = pivot - first;
      const std::size_t right_count = count - left_count - 1;

      if (left_count < right_count) {
        introsort(first, left_count, depth_limit);
        first = pivot + 1;
        count = right_count;
      } else {
        introsort(pivot + 1, right_count, depth_limit);
        count = left_count;
      }
    }
  }

  // Finishes the job: introsort left every element within its short partition, so each moves
  // only a few places.
  void insertion_sort(const std::size_t count) const noexcept {
    for (std::size_t i = 1; i < count; ++i) {
      for (std::size_t j = i; j > 0 && less(j, j - 1); --j) {
        swap(j, j - 1);
      }
    }
  }
};
}  // namespace

void qsort(void* ptr, const std::size_t count, const std::size_t size,
           const Compare compare) noexcept {
  if (count < 2 || size == 0) {
    return;
  }

  const Sorter sorter{ptr, size, compare};
  sorter.introsort(0, count, 2 * static_cast<std::size_t>(std::bit_width(count)));
  sorter.insertion_sort(count);
}
}  // namespace klib
