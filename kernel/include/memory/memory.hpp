/// @file memory.hpp
/// @brief Entry point and basic constants of the memory management subsystem.
///
/// The subsystem is built in three layers, brought up in this order by initialize():
///
///  1. PMM (`pmm.hpp`): a NUMA-aware buddy allocator over the `Page` array (vmemmap). It
///     hands out naturally aligned blocks of 2^order physical pages.
///  2. Heap (`heap.hpp`): a SLUB-style object allocator on top of the PMM, providing
///     `KmemCache`, `kmalloc()` and the global `operator new`/`delete`.
///  3. VMM (`vmm.hpp`): page tables (`PageMap`), TLB maintenance and address spaces made of
///     VMAs backed by `VMObject`s.
///
/// Each layer only depends on the ones below it, with one wrinkle: the PMM bootstraps its own
/// metadata with a private bump allocator because the heap does not exist yet at that point.

#pragma once

#include <cstddef>

namespace kernel::memory {
inline constexpr std::size_t PAGE_SHIFT_4KB = 12;
inline constexpr std::size_t PAGE_SHIFT_2MB = 21;
inline constexpr std::size_t PAGE_SHIFT_1GB = 30;

inline constexpr std::size_t PAGE_SIZE = 1ul << PAGE_SHIFT_4KB;
inline constexpr std::size_t PAGE_SIZE_2MB = 1ul << PAGE_SHIFT_2MB;
inline constexpr std::size_t PAGE_SIZE_1GB = 1ul << PAGE_SHIFT_1GB;

/// @brief Brings up the PMM, the kernel heap and the kernel address space, in that order.
///
/// Must run on the BSP after the per-CPU area and ACPI tables are available (the PMM reads
/// SRAT/SLIT/HMAT to discover NUMA topology). On return the CPU is running on the kernel's own
/// page tables rather than the ones Limine built.
void initialize();
}  // namespace kernel::memory

namespace kernel {
/// @brief Size of a kernel thread stack.
inline constexpr std::size_t KSTACK_SIZE = 4 * memory::PAGE_SIZE;
}  // namespace kernel
