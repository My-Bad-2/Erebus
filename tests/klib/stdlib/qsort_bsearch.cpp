#include <stdlib.h>

#include "ktest.hpp"

namespace ktest {
namespace {
std::size_t g_comparisons = 0;

int compare_ints(const void* a, const void* b) noexcept {
  ++g_comparisons;
  const int x = *static_cast<const int*>(a);
  const int y = *static_cast<const int*>(b);
  return (x > y) - (x < y);
}

constexpr std::size_t LARGE = 100'000;
int g_data[LARGE];
std::uint32_t g_counts[1024];

[[nodiscard]] bool is_sorted(const int* data, const std::size_t n) noexcept {
  for (std::size_t i = 1; i < n; ++i) {
    if (data[i - 1] > data[i]) {
      return false;
    }
  }

  return true;
}

// Values below 1024, counted, so that a sort that loses or invents an element shows up.
void count_values(const int* data, const std::size_t n, const int sign) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    g_counts[data[i]] += static_cast<std::uint32_t>(sign);
  }
}

[[nodiscard]] bool counts_balance() noexcept {
  for (const std::uint32_t count : g_counts) {
    if (count != 0) {
      return false;
    }
  }

  return true;
}

// Sorts data[0, n) and checks that the result is sorted and holds the same values, using at
// most `budget` comparisons.
void sort_and_check(const std::size_t n, const std::size_t budget) noexcept {
  for (std::uint32_t& count : g_counts) {
    count = 0;
  }
  count_values(g_data, n, 1);

  g_comparisons = 0;
  klib::qsort(g_data, n, sizeof(int), compare_ints);

  count_values(g_data, n, -1);
  KEXPECT(is_sorted(g_data, n));
  KEXPECT(counts_balance());
  KEXPECT_LE(g_comparisons, budget);
}

struct Wide {
  char padding[17];
  int key;
};

int compare_wide(const void* a, const void* b) noexcept {
  return static_cast<const Wide*>(a)->key - static_cast<const Wide*>(b)->key;
}
}  // namespace

KTEST(Qsort, sorts_every_short_length) {
  Rng rng{7};
  for (std::size_t n = 0; n <= 40; ++n) {
    for (std::size_t i = 0; i < n; ++i) {
      g_data[i] = static_cast<int>(rng.below(10));
    }
    sort_and_check(n, 1000);
  }
}

KTEST(Qsort, survives_all_equal_keys) {
  for (std::size_t i = 0; i < 5000; ++i) {
    g_data[i] = 42;
  }
  sort_and_check(5000, 3 * 5000 * 13);
}

// Introsort once only sorted a prefix and left the rest to the final insertion sort, which
// made qsort quadratic: about n^2 / 4 comparisons on random input.
KTEST(Qsort, stays_n_log_n_on_random_input) {
  Rng rng{42};
  for (int& value : g_data) {
    value = static_cast<int>(rng.below(1024));
  }

  // n log2 n is about 1.7 million here.
  sort_and_check(LARGE, 3 * LARGE * 17);
}

KTEST(Qsort, stays_n_log_n_on_ordered_input) {
  constexpr std::size_t N = 5000;
  // Ascending, descending, and up then down again (an "organ pipe").
  for (int shape = 0; shape < 3; ++shape) {
    for (std::size_t i = 0; i < N; ++i) {
      const auto up = static_cast<int>(i * 1024 / N);
      const auto down = static_cast<int>((N - 1 - i) * 1024 / N);
      g_data[i] = shape == 0 ? up : shape == 1 ? down : (i < N / 2 ? up : down);
    }
    sort_and_check(N, 3 * N * 13);
  }
}

KTEST(Qsort, handles_odd_element_sizes) {
  Wide data[] = {{"a", 99}, {"b", 1}, {"c", 50}, {"d", 42}};
  klib::qsort(data, 4, sizeof(Wide), compare_wide);
  KEXPECT_EQ(data[0].key, 1);
  KEXPECT_EQ(data[1].key, 42);
  KEXPECT_EQ(data[2].key, 50);
  KEXPECT_EQ(data[3].key, 99);
  KEXPECT_EQ(data[0].padding[0], 'b');
}

KTEST(Bsearch, finds_present_keys_only) {
  const int data[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
  for (int key = -5; key <= 95; ++key) {
    const auto* found =
        static_cast<const int*>(klib::bsearch(&key, data, 10, sizeof(int), compare_ints));
    if (key % 10 == 0 && key >= 0 && key <= 90) {
      KREQUIRE_NE(found, nullptr);
      KEXPECT_EQ(*found, key);
    } else {
      KEXPECT_EQ(found, nullptr);
    }
  }

  const int key = 30;
  KEXPECT_EQ(klib::bsearch(&key, data, 0, sizeof(int), compare_ints), nullptr);
}

// The gallop at the start must not run off either end of a large array.
KTEST(Bsearch, handles_both_ends_of_a_large_array) {
  for (std::size_t i = 0; i < 1000; ++i) {
    g_data[i] = static_cast<int>(i * 2);
  }

  const int first = 0;
  const int last = 1998;
  const int beyond = 2000;
  const int odd = 999;
  const auto find = [](const int& key) {
    return static_cast<const int*>(klib::bsearch(&key, g_data, 1000, sizeof(int), compare_ints));
  };

  KEXPECT_EQ(find(first), &g_data[0]);
  KEXPECT_EQ(find(last), &g_data[999]);
  KEXPECT_EQ(find(beyond), nullptr);
  KEXPECT_EQ(find(odd), nullptr);
}
}  // namespace ktest
