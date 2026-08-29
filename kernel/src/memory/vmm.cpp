#include "memory/vmm.hpp"

#include "hal/percpu.hpp"
#include "utils/logger.hpp"

extern "C" {
extern std::uint8_t __image_base[];
extern std::uint8_t __limine_requests_start[];
extern std::uint8_t __limine_requests_end[];
extern std::uint8_t __text_start[];
extern std::uint8_t __text_end[];
extern std::uint8_t __rodata_start[];
extern std::uint8_t __data_start[];
extern std::uint8_t __bss_end[];
extern std::uint8_t __init_start[];
extern std::uint8_t __init_end[];
}

namespace kernel::memory::vmm {
namespace {
PageMap *kernel_pagemap = nullptr;

[[nodiscard]] constexpr CacheMode determine_cache_mode(std::uint64_t limine_type) noexcept {
  switch (limine_type) {
  case LIMINE_MEMMAP_USABLE:
  case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
  case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
  case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    return CacheMode::WriteBack;
  case LIMINE_MEMMAP_FRAMEBUFFER:
    return CacheMode::WriteCombining;
  case LIMINE_MEMMAP_ACPI_NVS:
  case LIMINE_MEMMAP_RESERVED:
  case LIMINE_MEMMAP_RESERVED_MAPPED:
  default:
    return CacheMode::UncacheableMinus;
  }
}
} // namespace

const PageMap *get_kernel_pagemap() noexcept { return kernel_pagemap; }

void initialize(std::span<limine_memmap_entry *> memmap) noexcept {
  initialize_hw();

  auto pagemap_res = PageMap::create();
  if (!pagemap_res) {
    utils::logger::fatal("Unable to create pagemap! error: {}\n", std::to_underlying(pagemap_res.error()));
  }

  kernel_pagemap = *pagemap_res;

  constexpr AccessFlags hhdm_flags = AccessFlags::Read | AccessFlags::Write | AccessFlags::Global;
  for (const limine_memmap_entry *entry : memmap) {
    if (entry->type == LIMINE_MEMMAP_BAD_MEMORY) {
      continue;
    }

    const auto phys_base = PhysicalAddress{entry->base}.align_down(PAGE_SIZE);
    const auto phys_end = PhysicalAddress{entry->base + entry->length}.align_up(PAGE_SIZE);

    if (phys_base >= phys_end) {
      continue;
    }

    const std::size_t aligned_length = phys_end - phys_base;
    const VirtualAddress virt_base = DirectMap::phys_to_virt(phys_base);
    const CacheMode cache = determine_cache_mode(entry->type);

    [[maybe_unused]] auto _ = kernel_pagemap->map_range(virt_base, phys_base, aligned_length, hhdm_flags, cache);
  }

  const std::uint64_t kernel_phys_base = boot::executable_address_request.response->physical_base;
  const std::uint64_t kernel_virt_base = boot::executable_address_request.response->virtual_base;

  auto map_kernel_section = [&](std::uint8_t *start, std::uint8_t *end, AccessFlags flags) {
    const auto start_v = VirtualAddress(start).align_down(PAGE_SIZE);
    const auto end_v = VirtualAddress(end).align_up(PAGE_SIZE);

    if (start_v >= end_v) {
      return;
    }

    const std::size_t size_bytes = end_v - start_v;
    const auto phys_start = PhysicalAddress{start_v.value() - kernel_virt_base + kernel_phys_base};

    [[maybe_unused]] auto _ = kernel_pagemap->map_range(VirtualAddress{start_v}, PhysicalAddress{phys_start},
                                                        size_bytes, flags | AccessFlags::Global, CacheMode::WriteBack);
  };

  map_kernel_section(__limine_requests_start, __limine_requests_end, AccessFlags::Read | AccessFlags::Write);
  map_kernel_section(__text_start, __text_end, AccessFlags::Read | AccessFlags::Execute);
  map_kernel_section(__rodata_start, __data_start, AccessFlags::Read);
  map_kernel_section(__data_start, __bss_end, AccessFlags::Read | AccessFlags::Write);
  map_kernel_section(__init_start, __init_end, AccessFlags::Read | AccessFlags::Write | AccessFlags::Execute);

  hw::percpu::pcid_manager().load(kernel_pagemap);
}
} // namespace kernel::memory::vmm