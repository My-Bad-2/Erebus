#include "ktest.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;
using memory::vmm::has_flag;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;
constexpr std::size_t WORDS_PER_PAGE = PAGE_SIZE / sizeof(std::uint64_t);
}  // namespace

KTEST(AddressSpace, pages_in_on_demand_and_protects) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();
  const auto va = ks->alloc(64 * PAGE_SIZE, RW);
  KREQUIRE(va.has_value());

  auto* words = va->as<volatile std::uint64_t>();
  for (std::size_t i = 0; i < 64; ++i) {
    words[i * WORDS_PER_PAGE] = i * 7;
  }
  for (std::size_t i = 0; i < 64; ++i) {
    KEXPECT_EQ(words[i * WORDS_PER_PAGE], i * 7);
  }

  KEXPECT(ks->protect(*va + 8 * PAGE_SIZE, 8 * PAGE_SIZE, AccessFlags::Read).has_value());
  const auto t = ks->pagemap().translate(*va + 9 * PAGE_SIZE);
  KEXPECT(t && !has_flag(t->flags, AccessFlags::Write));
  KEXPECT(ks->protect(*va + 8 * PAGE_SIZE, 8 * PAGE_SIZE, RW).has_value());
  words[9 * WORDS_PER_PAGE] = 1234;
  KEXPECT(ks->free(*va).has_value());
}

KTEST(AddressSpace, aliases_populated_memory) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();
  const auto pv = ks->alloc(16 * PAGE_SIZE, RW | AccessFlags::Populate);
  KREQUIRE(pv.has_value());
  pv->as<std::uint64_t>()[3 * WORDS_PER_PAGE] = 0xABCDEF;

  const auto alias = ks->create_alias(*pv, 16 * PAGE_SIZE, RW);
  KREQUIRE(alias.has_value());
  KEXPECT_EQ(alias->as<std::uint64_t>()[3 * WORDS_PER_PAGE], 0xABCDEFu);
  KEXPECT(ks->destroy_alias(*alias).has_value());
  KEXPECT(ks->free(*pv).has_value());
}

KTEST(AddressSpace, splits_merges_locks_evicts_and_reallocates) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();
  const auto big = ks->alloc(32 * PAGE_SIZE, RW);
  KREQUIRE(big.has_value());

  for (std::size_t i = 0; i < 4; ++i) {
    KEXPECT(
        ks->protect(*big + (4 + 8 * i) * PAGE_SIZE, 2 * PAGE_SIZE, AccessFlags::Read).has_value());
  }
  KEXPECT(ks->protect(*big, 32 * PAGE_SIZE, RW).has_value());

  // Protecting slices split the area; protecting it all alike must merge it back.
  std::size_t pieces = 0;
  ks->for_each_vma([&](const memory::vmm::VMArea& v) {
    pieces += (v.start >= *big && v.start < *big + 32 * PAGE_SIZE) ? 1 : 0;
  });
  KEXPECT_EQ(pieces, 1u);

  big->as<volatile std::uint64_t>()[0] = 77;
  KEXPECT(ks->lock_memory(*big, 32 * PAGE_SIZE).has_value());
  KEXPECT(ks->pagemap().translate(*big + 31 * PAGE_SIZE).has_value());
  KEXPECT(!ks->evict_page(*big).has_value());
  KEXPECT(ks->unlock_memory(*big, 32 * PAGE_SIZE).has_value());
  KEXPECT(ks->evict_page(*big).has_value());
  KEXPECT_EQ(big->as<volatile std::uint64_t>()[0], 77u);

  const auto grown = ks->realloc(*big, 64 * PAGE_SIZE);
  KREQUIRE(grown.has_value());
  KEXPECT_EQ(grown->as<volatile std::uint64_t>()[0], 77u);
  grown->as<volatile std::uint64_t>()[63 * WORDS_PER_PAGE] = 99;
  KEXPECT(ks->free(*grown).has_value());
}

// 1 TiB at a time is more than a whole 32 TiB region after a few dozen rounds, so this only
// passes if freed ranges are found again.
KTEST(AddressSpace, reuses_freed_ranges) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();
  for (int i = 0; i < 100; ++i) {
    const auto huge = ks->alloc(1024 * memory::PAGE_SIZE_1GB, RW);
    KREQUIRE(huge.has_value());
    KREQUIRE(ks->free(*huge).has_value());
  }
}
}  // namespace kernel::testing
