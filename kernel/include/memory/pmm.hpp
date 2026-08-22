#pragma once

#include "address.hpp"
#include "boot/limine.h"
#include "memory.hpp"
#include "pmm/router.hpp"

namespace kernel::memory::pmm {
using AllocResult = std::optional<PhysicalAddress>;

[[nodiscard, gnu::always_inline]] inline PhysicalAddress page_to_phys(const Page *pg) noexcept {
  const std::uint64_t pfn = page_to_pfn(pg);
  return PhysicalAddress{pfn * PAGE_SIZE};
}

[[nodiscard, gnu::always_inline]] inline Page *phys_to_page(const PhysicalAddress phys) noexcept {
  const std::uint64_t pfn = phys.value() / PAGE_SIZE;
  return pfn_to_page(pfn);
}

[[nodiscard]] AllocResult alloc_pages(PageMobility mobility, std::uint8_t order = 0) noexcept;
[[nodiscard]] AllocResult alloc_pages_zeroed(PageMobility mobility, std::uint8_t order = 0) noexcept;
[[nodiscard]] AllocResult alloc_pages_on_node(std::uint32_t target_node, PageMobility mobility,
                                              std::uint8_t order = 0) noexcept;
std::size_t alloc_pages_bulk(PageMobility mobility, std::uint8_t order, std::span<PhysicalAddress> out_buffer) noexcept;

void free_pages(PhysicalAddress phys, std::uint8_t order = 0) noexcept;
void free_pages_bulk(std::span<const PhysicalAddress> addresses, std::uint8_t order = 0) noexcept;

void print_stats() noexcept;
void initialize(std::span<limine_memmap_entry *> memmap, std::uint32_t total_cpus) noexcept;
} // namespace kernel::memory::pmm