#include "ktest.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::DirectMap;
using memory::PAGE_SIZE;
using memory::PhysicalAddress;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;
}  // namespace

// Every layer is drained until it reports failure, then everything is handed back, and the
// layers must work again. The allocations are chained through their own first word, so that
// no bookkeeping memory is needed while there is none.
KTEST(OutOfMemory, every_layer_fails_cleanly_and_recovers) {
  std::size_t pages = 0;
  PhysicalAddress page_chain{};
  while (const auto p = memory::pmm::alloc_pages(memory::pmm::PageMobility::Movable, 0)) {
    *DirectMap::phys_to_virt(*p).as<std::uint64_t>() = page_chain.value();
    page_chain = *p;
    ++pages;
  }

  void* object_chain = nullptr;
  while (void* o = memory::heap::kmalloc(256)) {
    *static_cast<void**>(o) = object_chain;
    object_chain = o;
  }

  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();
  std::uintptr_t range_chain = 0;
  while (true) {
    const auto r = ks->alloc(16 * PAGE_SIZE, RW | AccessFlags::Populate);
    if (!r) {
      KEXPECT_EQ(r.error(), Error::OutOfMemory);
      break;
    }

    *r->as<std::uintptr_t>() = range_chain;
    range_chain = r->value();
  }

  KEXPECT_GT(pages, 0u);

  while (range_chain != 0) {
    const std::uintptr_t next = *reinterpret_cast<std::uintptr_t*>(range_chain);
    KEXPECT(ks->free(VirtualAddress{range_chain}).has_value());
    range_chain = next;
  }

  while (object_chain != nullptr) {
    void* next = *static_cast<void**>(object_chain);
    memory::heap::kfree(object_chain);
    object_chain = next;
  }

  while (page_chain.value() != 0) {
    const PhysicalAddress next{*DirectMap::phys_to_virt(page_chain).as<std::uint64_t>()};
    memory::pmm::free_pages(page_chain, 0);
    page_chain = next;
  }

  const auto after = ks->alloc(64 * PAGE_SIZE, RW | AccessFlags::Populate);
  KREQUIRE(after.has_value());
  KEXPECT(ks->free(*after).has_value());
}
}  // namespace kernel::testing
