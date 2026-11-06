#include "ktest.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;
using memory::vmm::PageSize;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;
}  // namespace

KTEST(PageMap, maps_huge_pages_and_splits_them_to_protect_one_page) {
  memory::vmm::PageMap& pm = memory::vmm::kernel_space()->pagemap();

  // A 4 MiB block, so that the range below is mapped with two 2 MiB pages.
  const auto block = memory::pmm::alloc_pages(memory::pmm::PageMobility::Movable, 10);
  KREQUIRE(block.has_value());

  const VirtualAddress va{0xFFFFC80000000000ul};
  constexpr std::size_t SIZE = 4ul << 20;
  KREQUIRE(pm.map_range(va, *block, SIZE, RW).has_value());

  const auto huge = pm.translate(va + 0x201000);
  KEXPECT(huge.has_value());
  KEXPECT(huge && huge->mapped_size == PageSize::Size2M);
  KEXPECT(huge && huge->phys_address == *block + 0x201000);

  // Protecting one page inside a huge page has to split it.
  KEXPECT(pm.protect_virtual_range(va + PAGE_SIZE, PAGE_SIZE, AccessFlags::Read).has_value());
  const auto split = pm.translate(va + PAGE_SIZE);
  KEXPECT(split.has_value());
  KEXPECT(split && split->mapped_size == PageSize::Size4K);
  KEXPECT(split && !memory::vmm::has_flag(split->flags, AccessFlags::Write));
  KEXPECT(split && split->phys_address == *block + PAGE_SIZE);

  KEXPECT(pm.unmap_range(va, SIZE).has_value());
  KEXPECT(!pm.translate(va + 0x300000).has_value());
  memory::pmm::free_pages(*block, 10);
}

// Nothing is mapped here: this must skip whole tables instead of walking 262144 pages.
KTEST(PageMap, unmaps_a_sparse_gigabyte) {
  memory::vmm::PageMap& pm = memory::vmm::kernel_space()->pagemap();
  KEXPECT(pm.unmap_range(VirtualAddress{0xFFFFD00000000000ul}, memory::PAGE_SIZE_1GB).has_value());
}
}  // namespace kernel::testing
