#include "memory/memory.hpp"

#include <span>

#include "boot/boot.hpp"
#include "hal/smp/smp.hpp"
#include "memory/address.hpp"
#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

namespace kernel::memory {
void initialize() {
  const std::span memmap(boot::memmap_request.response->entries,
                         boot::memmap_request.response->entry_count);
  const std::uint32_t total_cpus = hw::smp::possible_cpus();

  DirectMap::initialize(boot::hhdm_request.response->offset);
  pmm::initialize(memmap, total_cpus);
  heap::initialize();
  vmm::initialize(memmap);
}
}  // namespace kernel::memory