#pragma once

#include "vmm/pagemap/pagemap.hpp"
#include <span>

namespace kernel::memory::vmm {
void initialize(std::span<limine_memmap_entry *> memmap) noexcept;
}