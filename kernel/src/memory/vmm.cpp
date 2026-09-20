#include "memory/vmm.hpp"

#include "hal/interrupt_manager.hpp"
#include "hal/percpu.hpp"
#include "memory/vmm/vma/vma.hpp"

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
AddressSpace *g_kernel_space{nullptr};
constexpr std::uintptr_t KERNEL_VMA_HEAP_ADDR{0xFFFFC00000000000};

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

struct PageFaultErrorCode {
private:
  std::uint32_t m_data;

public:
  explicit PageFaultErrorCode(const std::uint32_t data) noexcept : m_data(data) {}

  BF_BIT_RO(present, 0)
  BF_BIT_RO(write, 1)
  BF_BIT_RO(user, 2)
  BF_BIT_RO(reserved_violation, 3)
  BF_BIT_RO(instruction_fetch, 4)
  BF_BIT_RO(protection_key, 5)
  BF_BIT_RO(shadow_stack, 6)
};

class PageFaultDispatcher {
  hw::interrupts::InterruptRegistration m_registration;
  static constexpr std::uint8_t PF_VECTOR = std::to_underlying(hw::interrupts::InterruptVector::PageFault);

  static hw::interrupts::InterruptStatus isr_trampoline(const hw::interrupts::Event &event, void *ctx) noexcept {
    auto *self = static_cast<PageFaultDispatcher *>(ctx);
    return self->dispatch(event);
  }

  hw::interrupts::InterruptStatus dispatch(const hw::interrupts::Event &event) noexcept {
    using namespace hw::interrupts;
    const auto fault_addr = VirtualAddress{event.payload};
    AddressSpace *curr_space = kernel_space();

    PageFaultErrorCode error_code{event.error_code};

    if (curr_space == nullptr) [[unlikely]] {
      return InterruptStatus::Unhandled;
    }

    AccessFlags fault_type = AccessFlags::None;
    if (error_code.get_write()) {
      fault_type |= AccessFlags::Write;
    } else {
      fault_type |= AccessFlags::Read;
    }

    if (error_code.get_user()) {
      fault_type |= AccessFlags::User;
    }

    if (error_code.get_instruction_fetch()) {
      fault_type |= AccessFlags::Execute;
    }

    const auto res = curr_space->handle_page_fault(fault_addr, fault_type);

    if (!res) {
      return InterruptStatus::Unhandled;
    }

    return InterruptStatus::Handled;
  }

public:
  PageFaultDispatcher() noexcept : m_registration(&PageFaultDispatcher::isr_trampoline, this) {
    using namespace hw::interrupts;
    InterruptManager::register_handler(PF_VECTOR, &m_registration);
  }
};

PageFaultDispatcher pf_dispatcher;
} // namespace

AddressSpace *kernel_space() noexcept { return g_kernel_space; }

void initialize(std::span<limine_memmap_entry *> memmap) noexcept {
  initialize_hw();
  AddressSpace::initialize();

  g_kernel_space = new AddressSpace(VirtualAddress{KERNEL_VMA_HEAP_ADDR});
  PageMap *kernel_pagemap = &g_kernel_space->pagemap();

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
  new (&pf_dispatcher) PageFaultDispatcher();
}
} // namespace kernel::memory::vmm