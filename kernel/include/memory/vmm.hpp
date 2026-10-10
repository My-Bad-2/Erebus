#pragma once

#include <span>

#include "vmm/pagemap/pagemap.hpp"

namespace kernel::memory::vmm {
void initialize(std::span<limine_memmap_entry*> memmap) noexcept;
}