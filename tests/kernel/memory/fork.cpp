#include "ktest.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::DirectMap;
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;
using memory::vmm::AddressSpace;
using memory::vmm::has_flag;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;
constexpr VirtualAddress USER_BASE{0x0000100000000000ul};
constexpr std::size_t PAGES = 8;

// The word at `v` in `as`, through the direct map: these page maps are never loaded into CR3,
// so the faults are driven by hand.
volatile std::uint64_t* word(AddressSpace* as, const VirtualAddress v) noexcept {
  const auto t = as->pagemap().translate(v);
  KEXPECT(t.has_value());
  static std::uint64_t nowhere = 0;
  return t ? DirectMap::phys_to_virt(t->phys_address).as<volatile std::uint64_t>() : &nowhere;
}
}  // namespace

KTEST(Fork, copies_on_write) {
  auto* parent = new AddressSpace(USER_BASE);
  const auto va = parent->alloc(PAGES * PAGE_SIZE, RW);
  KREQUIRE(va.has_value());

  for (std::size_t i = 0; i < PAGES; ++i) {
    KREQUIRE(parent->handle_page_fault(*va + i * PAGE_SIZE, AccessFlags::Write).has_value());
    *word(parent, *va + i * PAGE_SIZE) = 1000 + i;
  }

  for (std::uint64_t generation = 0; generation < 3; ++generation) {
    auto* child = new AddressSpace(USER_BASE);
    KREQUIRE(parent->clone_into(*child).has_value());

    const auto pt = parent->pagemap().translate(*va);
    KREQUIRE(pt.has_value());
    KEXPECT(!has_flag(pt->flags, AccessFlags::Write));

    // A read shares the parent's page; a write gets the child a copy of its own.
    KEXPECT(child->handle_page_fault(*va, AccessFlags::Read).has_value());
    auto ct = child->pagemap().translate(*va);
    KEXPECT(ct && ct->phys_address == pt->phys_address && !has_flag(ct->flags, AccessFlags::Write));

    KEXPECT(child->handle_page_fault(*va, AccessFlags::Write).has_value());
    ct = child->pagemap().translate(*va);
    KEXPECT(ct && ct->phys_address != pt->phys_address && has_flag(ct->flags, AccessFlags::Write));
    KEXPECT_EQ(*word(child, *va), 1000u);
    *word(child, *va) = 5555;
    KEXPECT_EQ(*word(parent, *va), 1000u);

    // And a write by the parent after the fork stays out of the child.
    KEXPECT(parent->handle_page_fault(*va + PAGE_SIZE, AccessFlags::Write).has_value());
    *word(parent, *va + PAGE_SIZE) = 2000 + generation;
    KEXPECT(child->handle_page_fault(*va + PAGE_SIZE, AccessFlags::Read).has_value());
    KEXPECT_EQ(*word(child, *va + PAGE_SIZE), generation == 0 ? 1001 : 2000 + generation - 1);

    // The kernel half is shared; destroying the child must not take it along.
    delete child;
    const auto probe = memory::vmm::kernel_space()->alloc(PAGE_SIZE, RW | AccessFlags::Populate);
    KREQUIRE(probe.has_value());
    probe->as<volatile std::uint64_t>()[0] = 1;
    KEXPECT(memory::vmm::kernel_space()->free(*probe).has_value());
  }

  for (std::size_t i = 2; i < PAGES; ++i) {
    KEXPECT(parent->handle_page_fault(*va + i * PAGE_SIZE, AccessFlags::Read).has_value());
    KEXPECT_EQ(*word(parent, *va + i * PAGE_SIZE), 1000 + i);
  }

  delete parent;
}
}  // namespace kernel::testing
