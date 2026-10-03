#include "hal/percpu.hpp"
#include "drivers/acpi.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts.hpp"
#include "hal/ioapic.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::hw::percpu {
namespace {
PerCpu bsp;
constexpr std::size_t CRITICAL_STACK_SIZE = 2 * memory::PAGE_SIZE;

void initialize_ioapic() noexcept {
  using namespace drivers::acpi;
  using namespace hw::interrupts;

  // Disable legacy 8259 PIC
  IoResource io(IoType::PMIO, 0x21, 1);
  io.write(0, 0xffu);
  io.write(0x80, 0xffu);

  auto madt_res = Table::find<ACPI_MADT_SIGNATURE>();
  if (!madt_res) {
    utils::logger::fatal("Unable to find MADT Table! Error: {}\n", madt_res.error());
  }

  const Table madt = std::move(*madt_res);
  const auto payload_res = madt.payload<acpi_madt>();
  if (!payload_res) {
    utils::logger::fatal("Unable to parse MADT Table! Error: {}\n", payload_res.error());
  }

  memory::vmm::AddressSpace *kernel_space = memory::vmm::kernel_space();

  for (const auto *hdr : *payload_res) {
    switch (hdr->type) {
    case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
      auto *ioapic = reinterpret_cast<const acpi_madt_ioapic *>(hdr);
      const memory::PhysicalAddress phys_base{ioapic->address};

      auto res = kernel_space->map_mmio(phys_base, memory::PAGE_SIZE);
      if (!res) {
        continue;
      }

      memory::VirtualAddress ioapic_base = *res;
      auto ioapic_res = io_apic_manager.register_controller(ioapic_base, ioapic->gsi_base);
      if (!ioapic_res) {
        utils::logger::fatal("Failed to register IOAPIC controller!\n");
      }

      break;
    }

    case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
      auto *iso = reinterpret_cast<const acpi_madt_interrupt_source_override *>(hdr);

      // Ensure it is an ISA bus (Bus 0).
      if (iso->bus == 0) {
        const auto res = io_apic_manager.register_isa_override(iso->source, iso->gsi, iso->flags);
        if (!res) {
          utils::logger::fatal("Failed to register ISA override! error: {}\n", res.error());
        }
      }

      break;
    }
    default:
      break;
    }
  }

  utils::logger::info("I/O APIC initialized!\n");
}
} // namespace

void early_initialize() noexcept {
  new (&bsp) PerCpu();
  bsp.gdt_table.load();

  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));

  [[maybe_unused]] auto _ = profile_manager.register_cpu();

  memory::vmm::early_initialize_hw();
}

void initialize_interrupts(memory::VirtualAddress rsp) noexcept {
  using namespace memory::vmm;
  auto kspace = kernel_space();

  const auto critical_stack_res =
      kspace->alloc(CRITICAL_STACK_SIZE, AccessFlags::Read | AccessFlags::Write | AccessFlags::Stack);
  if (!critical_stack_res) {
    utils::logger::fatal("Failed to allocate critical stack!\n");
  }

  bsp.gdt_table.set_interrupt_stack(gdt::IstIndex::Ist1, *critical_stack_res);
  bsp.gdt_table.set_kernel_stack(rsp);
  interrupts::initialize(rsp, *critical_stack_res);
  initialize_ioapic();
}
} // namespace kernel::hw::percpu