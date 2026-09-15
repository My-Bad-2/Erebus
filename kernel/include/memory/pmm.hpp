#pragma once

#include "address.hpp"
#include "boot/limine.h"
#include "memory.hpp"
#include "pmm/router.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
using AllocResult = std::optional<PhysicalAddress>;

namespace detail {
extern Page *g_vmemmap_base;
extern std::uint64_t g_max_pfn;
} // namespace detail

[[nodiscard, gnu::always_inline]] inline std::uint64_t page_to_pfn(const Page *page) noexcept {
  return static_cast<std::uint64_t>(page - detail::g_vmemmap_base);
}

[[nodiscard, gnu::always_inline]] inline Page *pfn_to_page(const std::uint64_t pfn) noexcept {
  if (pfn >= detail::g_max_pfn) [[unlikely]] {
    utils::logger::fatal("PMM: pfn_to_page out of bounds (PFN: {}, MAX: {})\n", pfn, detail::g_max_pfn);
  }

  return detail::g_vmemmap_base + pfn;
}

[[nodiscard, gnu::always_inline]] inline PhysicalAddress page_to_phys(const Page *pg) noexcept {
  return PhysicalAddress{page_to_pfn(pg) * PAGE_SIZE};
}

[[nodiscard, gnu::always_inline]] inline Page *phys_to_page(const PhysicalAddress phys) noexcept {
  return pfn_to_page(phys.value() / PAGE_SIZE);
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