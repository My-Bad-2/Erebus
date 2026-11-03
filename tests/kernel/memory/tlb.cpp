#include <atomic>

#include "ktest.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::DirectMap;
using memory::PhysicalAddress;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;

VirtualAddress g_page{};
PhysicalAddress g_old_frame{};
PhysicalAddress g_new_frame{};
}  // namespace

// Every CPU caches a translation; CPU 0 then moves the page to another frame. The remap has to
// reach every TLB, or someone keeps reading the old frame.
KTEST_ALL_CPUS(Tlb, remapping_reaches_every_cpu, cpu) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();

  if (cpu == 0) {
    const auto page = ks->alloc(memory::PAGE_SIZE, RW | AccessFlags::Populate);
    KEXPECT(page.has_value());
    g_page = page.value_or(VirtualAddress{});
    if (g_page) {
      g_page.as<volatile std::uint64_t>()[0] = 111;
    }
  }
  sync_cpus();
  KREQUIRE(static_cast<bool>(g_page));

  KEXPECT_EQ(g_page.as<volatile std::uint64_t>()[0], 111u);
  sync_cpus();

  if (cpu == 0) {
    const auto old_translation = ks->pagemap().translate(g_page);
    const auto new_frame = memory::pmm::alloc_pages(memory::pmm::PageMobility::Unmovable, 0);
    KEXPECT(old_translation.has_value());
    KEXPECT(new_frame.has_value());
    if (old_translation && new_frame) {
      g_old_frame = old_translation->phys_address;
      g_new_frame = *new_frame;
      *DirectMap::phys_to_virt(g_new_frame).as<std::uint64_t>() = 222;
      KEXPECT(ks->pagemap().remap(g_page, g_new_frame, RW).has_value());
    }
  }
  sync_cpus();

  KEXPECT_EQ(g_page.as<volatile std::uint64_t>()[0], 222u);
  sync_cpus();

  if (cpu == 0) {
    KEXPECT(ks->pagemap().remap(g_page, g_old_frame, RW).has_value());
    memory::pmm::free_pages(g_new_frame, 0);
    KEXPECT(ks->free(g_page).has_value());
    g_page = VirtualAddress{};
  }
}
}  // namespace kernel::testing
