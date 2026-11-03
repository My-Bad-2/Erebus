#include "memory/pmm.hpp"

#include "ktest.hpp"
#include "memory/memory.hpp"

namespace kernel::testing {
namespace {
using memory::DirectMap;
using memory::PhysicalAddress;
using memory::pmm::PageMobility;

PhysicalAddress g_pages[2048];
PhysicalAddress g_mixed[512];
std::uint8_t g_orders[512];
}  // namespace

KTEST(Pmm, hands_out_distinct_order_zero_pages) {
  // Each page holds its own address; a page handed out twice would be overwritten.
  for (PhysicalAddress& page : g_pages) {
    const auto r = memory::pmm::alloc_pages(PageMobility::Unmovable, 0);
    KREQUIRE(r.has_value());
    page = *r;
    *DirectMap::phys_to_virt(page).as<std::uint64_t>() = page.value();
  }

  for (const PhysicalAddress& page : g_pages) {
    KEXPECT_EQ(*DirectMap::phys_to_virt(page).as<std::uint64_t>(), page.value());
    memory::pmm::free_pages(page, 0);
  }
}

KTEST(Pmm, aligns_blocks_of_every_order) {
  Rng rng{0x9E3779B97F4A7C15ull};
  for (int round = 0; round < 8; ++round) {
    for (std::size_t i = 0; i < 512; ++i) {
      g_orders[i] = static_cast<std::uint8_t>(rng.below(7));
      const auto r = memory::pmm::alloc_pages(PageMobility::Movable, g_orders[i]);
      KREQUIRE(r.has_value());
      KEXPECT(r->is_aligned(memory::PAGE_SIZE << g_orders[i]));
      g_mixed[i] = *r;
    }

    for (std::size_t i = 0; i < 512; ++i) {
      memory::pmm::free_pages(g_mixed[i], g_orders[i]);
    }
  }
}

// The reclaimable zone starts out empty, so this has to steal from the others.
KTEST(Pmm, steals_for_an_empty_zone) {
  for (int i = 0; i < 4096; ++i) {
    const auto r = memory::pmm::alloc_pages(PageMobility::Reclaimable, 0);
    KREQUIRE(r.has_value());
    memory::pmm::free_pages(*r, 0);
  }
}

KTEST(Pmm, allocates_and_frees_in_bulk) {
  const auto got = memory::pmm::alloc_pages_bulk(PageMobility::Reclaimable, 1, g_pages);
  KEXPECT_EQ(got, std::size(g_pages));
  memory::pmm::free_pages_bulk(std::span<const PhysicalAddress>{g_pages, got}, 1);
}
}  // namespace kernel::testing
