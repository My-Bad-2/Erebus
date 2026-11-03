#include "memory/vmm.hpp"

#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/percpu.hpp"
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
AddressSpace* g_kernel_space{nullptr};
constexpr std::uintptr_t KERNEL_VMA_HEAP_ADDR{0xFFFFC00000000000};

[[nodiscard]] constexpr CacheMode determine_cache_mode(const std::uint64_t limine_type) noexcept {
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
  static constexpr std::uint8_t PF_VECTOR =
      std::to_underlying(hw::interrupts::InterruptVector::PageFault);

  static hw::interrupts::InterruptStatus isr_trampoline(const hw::interrupts::Event& event,
                                                        void* ctx) noexcept {
    auto* self = static_cast<PageFaultDispatcher*>(ctx);
    return self->dispatch(event);
  }

  hw::interrupts::InterruptStatus dispatch(const hw::interrupts::Event& event) noexcept {
    using namespace hw::interrupts;

    // TODO: Fault in the current task's address space once there are tasks.
    AddressSpace* curr_space = kernel_space();
    if (curr_space == nullptr) [[unlikely]] {
      return InterruptStatus::Unhandled;
    }

    const auto fault_addr = VirtualAddress{event.payload};
    const PageFaultErrorCode error_code{event.error_code};

    // A reserved-bit violation means corrupted page tables, not something to page in.
    if (error_code.get_reserved_violation()) [[unlikely]] {
      return InterruptStatus::Unhandled;
    }

    AccessFlags fault_type = error_code.get_write() ? AccessFlags::Write : AccessFlags::Read;

    if (error_code.get_user()) {
      fault_type |= AccessFlags::User;
    }

    if (error_code.get_instruction_fetch()) {
      fault_type |= AccessFlags::Execute;
    }

    // The fault arrives with interrupts off. Resolving it can mean waiting for the address
    // space lock, and whoever holds that may be waiting for this CPU to answer a TLB
    // shootdown, so give the faulting context its interrupts back while we work.
    const bool reenable = (event.rflags & hw::irq::RFLAGS_IF) != 0;
    if (reenable) {
      hw::irq::enable();
    }

    const auto res = curr_space->handle_page_fault(fault_addr, fault_type);

    if (reenable) {
      hw::irq::disable();
    }

    return res ? InterruptStatus::Handled : InterruptStatus::Unhandled;
  }

 public:
  constexpr PageFaultDispatcher() noexcept
      : m_registration(&PageFaultDispatcher::isr_trampoline, this) {}

  void install() noexcept {
    hw::interrupts::InterruptManager::register_handler(PF_VECTOR, &m_registration);
  }
};

constinit PageFaultDispatcher pf_dispatcher;

void map_or_die(PageMap& pagemap, const VirtualAddress virt, const PhysicalAddress phys,
                const std::size_t size, const AccessFlags flags, const CacheMode cache) noexcept {
  if (const auto res = pagemap.map_range(virt, phys, size, flags, cache); !res) [[unlikely]] {
    utils::logger::fatal("VMM: failed to map {} -> {} ({} bytes): error {}\n", virt, phys, size,
                         std::to_underlying(res.error()));
  }
}
}  // namespace

AddressSpace* kernel_space() noexcept { return g_kernel_space; }

std::expected<VirtualAddress, Error> map_device_registers(const PhysicalAddress phys,
                                                          const std::size_t size) noexcept {
  if (size == 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const PhysicalAddress base = phys.align_down(PAGE_SIZE);
  const std::size_t offset = phys.value() - base.value();
  const std::size_t span = utils::maths::align_up(offset + size, PAGE_SIZE);

  return g_kernel_space->map_mmio(base, span).transform([offset](const VirtualAddress va) {
    return va + static_cast<VirtualAddress::OffsetType>(offset);
  });
}

void initialize(const std::span<limine_memmap_entry*> memmap) noexcept {
  initialize_hw();
  AddressSpace::initialize();

  g_kernel_space = new AddressSpace(VirtualAddress{KERNEL_VMA_HEAP_ADDR});
  PageMap& kernel_pagemap = g_kernel_space->pagemap();

  // Direct map of everything the firmware told us about. Entries are sorted and do not overlap,
  // but rounding to pages can make neighbours share a page; the first entry gets it.
  constexpr AccessFlags hhdm_flags = AccessFlags::Read | AccessFlags::Write;
  PhysicalAddress mapped_until{};

  for (const limine_memmap_entry* entry : memmap) {
    if (entry->type == LIMINE_MEMMAP_BAD_MEMORY) {
      continue;
    }

    const auto phys_base =
        std::max(PhysicalAddress{entry->base}.align_down(PAGE_SIZE), mapped_until);
    const auto phys_end = PhysicalAddress{entry->base + entry->length}.align_up(PAGE_SIZE);

    if (phys_base >= phys_end) {
      continue;
    }

    map_or_die(kernel_pagemap, DirectMap::phys_to_virt(phys_base), phys_base,
               static_cast<std::size_t>(phys_end - phys_base), hhdm_flags,
               determine_cache_mode(entry->type));
    mapped_until = phys_end;
  }

  const std::uint64_t kernel_phys_base = boot::executable_address_request.response->physical_base;
  const std::uint64_t kernel_virt_base = boot::executable_address_request.response->virtual_base;

  const auto map_kernel_section = [&](std::uint8_t* start, std::uint8_t* end,
                                      const AccessFlags flags) {
    const auto start_v = VirtualAddress(start).align_down(PAGE_SIZE);
    const auto end_v = VirtualAddress(end).align_up(PAGE_SIZE);

    if (start_v >= end_v) {
      return;
    }

    const auto phys_start = PhysicalAddress{start_v.value() - kernel_virt_base + kernel_phys_base};
    map_or_die(kernel_pagemap, start_v, phys_start, static_cast<std::size_t>(end_v - start_v),
               flags, CacheMode::WriteBack);
  };

  map_kernel_section(__limine_requests_start, __limine_requests_end,
                     AccessFlags::Read | AccessFlags::Write);
  map_kernel_section(__text_start, __text_end, AccessFlags::Read | AccessFlags::Execute);
  map_kernel_section(__rodata_start, __data_start, AccessFlags::Read);
  map_kernel_section(__data_start, __bss_end, AccessFlags::Read | AccessFlags::Write);
  map_kernel_section(__init_start, __init_end, AccessFlags::Read);

  hw::percpu::pcid_manager().load(&kernel_pagemap);
  pf_dispatcher.install();
}
}  // namespace kernel::memory::vmm
