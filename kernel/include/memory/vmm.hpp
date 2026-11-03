/// @file vmm.hpp
/// @brief Bring-up of the virtual memory manager.

#pragma once

#include <span>

#include "boot/limine.h"
#include "vmm/pagemap/pagemap.hpp"

namespace kernel::memory::vmm {
/// @brief Builds the kernel address space and switches the CPU onto it.
///
/// The new page tables contain the direct map of every memory map entry (write-back for RAM,
/// write-combining for framebuffers, UC- for everything else) and the kernel image with
/// per-section permissions (W^X except for `.init`). The page fault handler is installed last.
void initialize(std::span<limine_memmap_entry*> memmap) noexcept;
}  // namespace kernel::memory::vmm
