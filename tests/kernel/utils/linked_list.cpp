#include "utils/linked_list.hpp"

#include <initializer_list>

#include "ktest.hpp"

namespace kernel::testing {
namespace {
struct Item : utils::SListNode<Item> {
  int value = 0;
};

using List = utils::SList<Item, void>;

// Whether the list holds exactly `expected`, in that order.
[[nodiscard]] bool holds(const List& list, const std::initializer_list<int> expected) noexcept {
  const int* next = expected.begin();
  for (const Item& item : list) {
    if (next == expected.end() || item.value != *next) {
      return false;
    }
    ++next;
  }

  return next == expected.end();
}

Item g_items[16];

void number_items() noexcept {
  for (int i = 0; i < 16; ++i) {
    g_items[i].value = i;
  }
}
}  // namespace

KTEST(SList, pushes_and_pops_at_the_front) {
  number_items();
  List list;
  KEXPECT(list.empty());

  list.push_front(g_items[1]);
  list.push_front(g_items[2]);
  list.push_front(g_items[3]);
  KEXPECT(holds(list, {3, 2, 1}));
  KEXPECT_EQ(list.front().value, 3);

  KEXPECT_EQ(list.pop_front(), &g_items[3]);
  KEXPECT(!g_items[3].is_linked());
  KEXPECT(holds(list, {2, 1}));

  list.clear();
  KEXPECT(list.empty());
  KEXPECT(!g_items[1].is_linked());
  KEXPECT(!g_items[2].is_linked());
}

// The last item of a list is the one a null-terminated list would get wrong.
KTEST(SList, knows_which_items_are_linked) {
  number_items();
  List list;
  for (Item& item : g_items) {
    KEXPECT(!item.is_linked());
    list.push_front(item);
  }

  KEXPECT(g_items[0].is_linked());
  KEXPECT(g_items[15].is_linked());
  list.clear();
  KEXPECT(!g_items[0].is_linked());
}

KTEST(SList, inserts_and_erases_after_a_position) {
  number_items();
  List list;
  list.insert_after(list.before_begin(), g_items[1]);
  const auto it = list.insert_after(list.begin(), g_items[3]);
  list.insert_after(list.begin(), g_items[2]);
  KEXPECT(holds(list, {1, 2, 3}));
  KEXPECT_EQ(it->value, 3);

  KEXPECT_EQ(list.erase_after(list.begin()), &g_items[2]);
  KEXPECT(holds(list, {1, 3}));
  list.clear();
}

KTEST(SList, removes_and_reverses) {
  number_items();
  List list;
  for (int i = 15; i >= 0; --i) {
    list.push_front(g_items[i]);
  }

  KEXPECT(list.remove(g_items[0]));
  KEXPECT(!g_items[0].is_linked());
  KEXPECT(!list.remove(g_items[0]));

  list.reverse();
  int expect = 15;
  for (const Item& item : list) {
    KEXPECT_EQ(item.value, expect--);
  }
  KEXPECT_EQ(expect, 0);
  list.clear();
}

KTEST(SList, moves_its_items) {
  number_items();
  List first;
  first.push_front(g_items[2]);
  first.push_front(g_items[1]);

  List second{static_cast<List&&>(first)};
  KEXPECT(first.empty());
  KEXPECT(holds(second, {1, 2}));

  first = static_cast<List&&>(second);
  KEXPECT(holds(first, {1, 2}));
  first.clear();
}
}  // namespace kernel::testing
