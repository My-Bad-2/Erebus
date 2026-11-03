#include "utils/vector.hpp"

#include "ktest.hpp"

namespace kernel::testing {
namespace {
// Counts live objects, so that leaks and double destruction show up.
struct Tracked {
  static inline std::int64_t live = 0;
  std::uint64_t value;

  explicit Tracked(const std::uint64_t v) noexcept : value(v) { ++live; }
  Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
  Tracked& operator=(Tracked&& other) noexcept {
    value = other.value;
    return *this;
  }
  ~Tracked() noexcept { --live; }
};
}  // namespace

KTEST(Vector, grows_inserts_and_erases_in_order) {
  utils::Vector<std::uint64_t> v;
  KEXPECT(v.empty());
  KEXPECT_EQ(v.data(), nullptr);

  for (std::uint64_t i = 0; i < 5000; ++i) {
    KREQUIRE(v.push_back(i).has_value());
  }

  KREQUIRE(v.insert(v.begin(), 111).has_value());
  KREQUIRE(v.insert(v.begin() + 2500, 222).has_value());
  KEXPECT_EQ(v[0], 111u);
  KEXPECT_EQ(v[1], 0u);
  KEXPECT_EQ(v[2500], 222u);
  KEXPECT_EQ(v[2501], 2499u);
  KEXPECT_EQ(v.back(), 4999u);

  (void)v.erase(v.begin());
  (void)v.erase(v.begin() + 2499);
  for (std::uint64_t i = 0; i < 5000; ++i) {
    KREQUIRE_EQ(v[i], i);
  }
}

// An argument that refers into the vector must survive the reallocation it triggers.
KTEST(Vector, emplaces_a_reference_to_its_own_element) {
  utils::Vector<std::uint64_t> v;
  for (std::uint64_t i = 0; i < 100; ++i) {
    KREQUIRE(v.push_back(i + 1).has_value());
  }

  KEXPECT(v.shrink_to_fit());
  KEXPECT_EQ(v.capacity(), v.size());
  KREQUIRE(v.emplace_back(v[0]).has_value());
  KEXPECT_EQ(v.back(), 1u);
}

KTEST(Vector, resizes_both_ways) {
  utils::Vector<std::uint64_t> v;
  for (std::uint64_t i = 0; i < 50; ++i) {
    KREQUIRE(v.push_back(i + 1).has_value());
  }

  KREQUIRE(v.resize(10).has_value());
  KEXPECT_EQ(v.size(), 10u);
  KREQUIRE(v.resize(20).has_value());
  // New elements are value-initialized.
  KEXPECT_EQ(v[19], 0u);
}

KTEST(Vector, constructs_and_destroys_each_element_once) {
  {
    utils::Vector<Tracked> t;
    for (std::uint64_t i = 0; i < 300; ++i) {
      KREQUIRE(t.emplace_back(i).has_value());
    }

    KREQUIRE(t.insert(t.begin() + 10, Tracked{7}).has_value());
    (void)t.erase(t.begin() + 3);
    (void)t.swap_erase(t.begin());
    KEXPECT_EQ(Tracked::live, 299);
  }

  KEXPECT_EQ(Tracked::live, 0);
}
}  // namespace kernel::testing
