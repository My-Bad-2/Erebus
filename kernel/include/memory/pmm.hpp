/// @file pmm.hpp
/// @brief Public interface of the physical memory manager.
///
/// The PMM hands out naturally aligned blocks of 2^order contiguous 4 KiB frames, from order 0
/// (one page) up to MAX_ORDER (1 GiB). Requests are routed to the NUMA node of the calling CPU
/// (see Router), then to the buddy zone of the requested PageMobility on that node
/// (see NumaNode and MobilityZone).
///
/// Every allocated block starts with a reference count of one on its head descriptor.
/// free_pages() drops that reference and only returns the block to the buddy allocator once the
/// count reaches zero, so a page can be shared by taking extra references with Page::inc_ref().

#pragma once

#include "address.hpp"
#include "boot/limine.h"
#include "memory.hpp"
#include "pmm/router.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
using AllocResult = std::optional<PhysicalAddress>;

namespace detail {
extern Page* g_vmemmap_base;
extern std::uint64_t g_max_pfn;
}  // namespace detail

[[nodiscard, gnu::always_inline]] inline bool pfn_valid(const std::uint64_t pfn) noexcept {
  return pfn < detail::g_max_pfn;
}

[[nodiscard, gnu::always_inline]] inline std::uint64_t page_to_pfn(const Page* page) noexcept {
  return static_cast<std::uint64_t>(page - detail::g_vmemmap_base);
}

/// @brief Descriptor of frame @p pfn. Out-of-range PFNs are a fatal error.
[[nodiscard, gnu::always_inline]] inline Page* pfn_to_page(const std::uint64_t pfn) noexcept {
  if (!pfn_valid(pfn)) [[unlikely]] {
    utils::logger::fatal("PMM: pfn_to_page out of bounds (PFN: {}, MAX: {})\n", pfn,
                         detail::g_max_pfn);
  }

  return detail::g_vmemmap_base + pfn;
}

[[nodiscard, gnu::always_inline]] inline PhysicalAddress page_to_phys(const Page* pg) noexcept {
  return PhysicalAddress{page_to_pfn(pg) * PAGE_SIZE};
}

[[nodiscard, gnu::always_inline]] inline Page* phys_to_page(const PhysicalAddress phys) noexcept {
  return pfn_to_page(phys.value() / PAGE_SIZE);
}

/// @brief Allocates 2^@p order pages from the local NUMA node, falling back to remote nodes.
/// @return The physical address of the block (aligned to its size), or std::nullopt.
[[nodiscard]] AllocResult alloc_pages(PageMobility mobility, std::uint8_t order = 0) noexcept;

/// @brief Like alloc_pages(), but the block is zero-filled through the direct map.
[[nodiscard]] AllocResult alloc_pages_zeroed(PageMobility mobility,
                                             std::uint8_t order = 0) noexcept;

/// @brief Like alloc_pages(), but prefers @p target_node instead of the caller's node.
[[nodiscard]] AllocResult alloc_pages_on_node(std::uint32_t target_node, PageMobility mobility,
                                              std::uint8_t order = 0) noexcept;

/// @brief Allocates up to `out_buffer.size()` blocks of the same order in one go.
/// @return Number of blocks written to @p out_buffer; less than requested means out of memory.
std::size_t alloc_pages_bulk(PageMobility mobility, std::uint8_t order,
                             std::span<PhysicalAddress> out_buffer) noexcept;

/// @brief Drops a reference to a block; the block is freed when the last one goes away.
/// @param phys  Address returned by one of the allocation functions.
/// @param order The order the block was allocated with.
/// @note Freeing a block that has no references left is a fatal error (double free).
void free_pages(PhysicalAddress phys, std::uint8_t order = 0) noexcept;

/// @brief free_pages() for every entry of @p addresses. Null addresses are skipped.
void free_pages_bulk(std::span<const PhysicalAddress> addresses, std::uint8_t order = 0) noexcept;

/// @brief Has every CPU give back the blocks it keeps from the zones of node @p node_id.
///
/// Any CPU can hold any node's pages, so this is an IPI to all of them. See
/// MobilityZone::drain_pcp_local().
/// @pre The calling thread can wait for other CPUs.
/// @return How many pages the zones got back.
std::size_t drain_cpu_caches(std::uint32_t node_id) noexcept;

/// @brief Logs per-node, per-zone free memory, fragmentation and watermarks.
void print_stats() noexcept;

/// @brief Builds the vmemmap, discovers NUMA topology and hands all usable RAM to the zones.
/// @param memmap     Limine's memory map.
/// @param total_cpus Number of CPUs, used to size the per-CPU page caches.
void initialize(std::span<limine_memmap_entry*> memmap, std::uint32_t total_cpus) noexcept;
}  // namespace kernel::memory::pmm
