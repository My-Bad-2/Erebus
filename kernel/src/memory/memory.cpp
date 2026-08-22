#include "memory/memory.hpp"
#include "boot/boot.hpp"
#include "memory/address.hpp"
#include "memory/pmm.hpp"
#include <span>

namespace kernel::memory {
void initialize() {
  std::span memmap(boot::memmap_request.response->entries, boot::memmap_request.response->entry_count);
  const std::uint32_t total_cpus = boot::mp_request.response->cpu_count;

  DirectMap::initialize(boot::hhdm_request.response->offset);
  pmm::initialize(memmap, total_cpus);
}
} // namespace kernel::memory