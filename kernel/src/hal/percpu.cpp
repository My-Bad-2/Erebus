#include "hal/percpu.hpp"
#include "drivers/acpi.hpp"
#include "hal/apic/ioapic.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::hw::percpu {
namespace {
PerCpu bsp;
utils::QSpinlock cpu_list_lock;
utils::Vector<PerCpu *> all_cpus;

constexpr std::size_t CRITICAL_STACK_SIZE = 2 * memory::PAGE_SIZE;
constexpr std::size_t AP_STACK_SIZE = 4 * memory::PAGE_SIZE;

memory::VirtualAddress global_lapic_base{};
std::uint32_t runqueue; // placeholder for now

void initialize_ioapic() noexcept {
  using namespace drivers::acpi;
  using namespace hw::apic;

  // Disable legacy 8259 PIC
  IoResource io(IoType::PMIO, 0x21, 1);
  io.write(0, 0xffu);
  io.write(0x80, 0xffu);

  auto madt_res = Table::find<ACPI_MADT_SIGNATURE>();
  if (!madt_res) {
    utils::logger::fatal("MADT Table missing: {}\n", madt_res.error());
  }

  const Table madt = std::move(*madt_res);
  const auto payload_res = madt.payload<acpi_madt>();
  if (!payload_res) {
    utils::logger::fatal("MADT Table invalid: {}\n", madt_res.error());
  }

  memory::vmm::AddressSpace *kspace = memory::vmm::kernel_space();

  for (const auto *hdr : *payload_res) {
    if (hdr->type == ACPI_MADT_ENTRY_TYPE_IOAPIC) {
      auto *ioapic = reinterpret_cast<const acpi_madt_ioapic *>(hdr);

      auto mapped_base = kspace->map_mmio(memory::PhysicalAddress{ioapic->address}, memory::PAGE_SIZE);
      if (mapped_base && !io_apic_manager.register_controller(*mapped_base, ioapic->gsi_base)) {
        utils::logger::fatal("Failed to register IOAPIC controller!\n");
      }
    } else if (hdr->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE) {
      auto *iso = reinterpret_cast<const acpi_madt_interrupt_source_override *>(hdr);

      // Ensure it is an ISA bus (Bus 0)
      if (iso->bus == 0 && !io_apic_manager.register_isa_override(iso->source, iso->gsi, iso->flags)) {
        utils::logger::fatal("Failed to register ISA override for source {}!\n", iso->source);
      }
    }
  }

  utils::logger::info("I/O APIC initialized!\n");
}

void initialize_lapic(apic::Lapic &lapic) noexcept {
  lapic.initialize(global_lapic_base, true);
  if (apic::io_apic_manager.supports_directed_eoi()) {
    lapic.enable_directed_eoi(true);
  }

  utils::logger::info("LAPIC initialized for CPU {}!\n", lapic.id());
}

void finalize_cpu_setup(PerCpu *cpu, const memory::VirtualAddress rsp) noexcept {
  using namespace memory::vmm;
  AddressSpace *kspace = kernel_space();

  const auto critical_stack = kspace->alloc(CRITICAL_STACK_SIZE, AccessFlags::Read | AccessFlags::Write |
                                                                     AccessFlags::Stack | AccessFlags::Populate);
  if (!critical_stack) {
    utils::logger::fatal("Failed to allocate critical stack for CPU {}!\n", cpu->topology.get_cpu_id());
  }

  cpu->gdt_table.set_interrupt_stack(gdt::IstIndex::Ist1, *critical_stack);
  cpu->gdt_table.set_kernel_stack(rsp);

  interrupts::initialize(rsp, *critical_stack);
  initialize_lapic(cpu->lapic);

  utils::IrqSaveGuard guard{cpu_list_lock};
  (void)all_cpus.push_back(cpu);
}

void initialize_bsp(volatile limine_mp_info *info, const memory::VirtualAddress rsp) noexcept {
  using namespace memory::vmm;

  bsp.topology.set_cpu_id(info->processor_id);

  // Map LAPIC MMIO globally for all CPUs to use
  const memory::PhysicalAddress phys_lapic_base{read::msr(apic::IA32_APIC_BASE)};
  auto lapic_base_res = kernel_space()->map_mmio(phys_lapic_base.align_down(memory::PAGE_SIZE), memory::PAGE_SIZE);
  if (!lapic_base_res) {
    utils::logger::fatal("Failed to map LAPIC base! error: {}\n", std::to_underlying(lapic_base_res.error()));
  }

  global_lapic_base = *lapic_base_res;
  initialize_ioapic();

  finalize_cpu_setup(&bsp, rsp);
}

void initialize_ap(volatile limine_mp_info *info, const memory::VirtualAddress rsp) noexcept {
  using namespace memory::vmm;

  // Temporarily adopt BSP's GS_BASE to allow subsystems to function
  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));

  auto percpu_page =
      kernel_space()->alloc(sizeof(PerCpu), AccessFlags::Read | AccessFlags::Write | AccessFlags::Populate);

  if (!percpu_page) {
    utils::logger::fatal("Failed to allocate percpu page for AP {}!\n", info->processor_id);
  }

  const auto cpu = new (percpu_page->as<PerCpu *>()) PerCpu();
  cpu->topology.set_cpu_id(info->processor_id);
  cpu->gdt_table.load();

  write::gs_base(percpu_page->value());

  (void)profile_manager.register_cpu();
  early_initialize_hw();

  finalize_cpu_setup(cpu, rsp);
}

void ap_main(volatile limine_mp_info *info) {
  memory::VirtualAddress rsp{info->extra_argument};

  initialize_ap(info, rsp);
  irq::enable();

  utils::logger::info("Hello, world from {}!\n", info->processor_id);
  cpu_idle_loop(&runqueue);
}

void ap_trampoline(volatile limine_mp_info *info) noexcept {
  // Sync the AP with the VMM pagemap
  write::cr3(CR3{memory::vmm::kernel_space()->pagemap().root_phys().value()});
  execute_on_new_stack(info->extra_argument, const_cast<limine_mp_info *>(info),
                       reinterpret_cast<void (*)(void *)>(ap_main));
}
} // namespace

void early_initialize() noexcept {
  new (&bsp) PerCpu();
  bsp.gdt_table.load();

  write::gs_base(reinterpret_cast<std::uintptr_t>(&bsp));

  (void)profile_manager.register_cpu();
  memory::vmm::early_initialize_hw();
}

void initialize(const memory::VirtualAddress bsp_rsp) noexcept {
  const auto *mp_resp = boot::mp_request.response;
  std::span cpus{mp_resp->cpus, mp_resp->cpu_count};

  const auto bsp_it = std::ranges::find(cpus, mp_resp->bsp_lapic_id, &limine_mp_info::lapic_id);
  limine_mp_info *bsp_info = (bsp_it != cpus.end()) ? *bsp_it : nullptr;

  if (bsp_info) {
    initialize_bsp(bsp_info, bsp_rsp);
  }

  // Wake APs
  for (auto *cpu : cpus) {
    if (cpu == bsp_info) {
      continue;
    }

    const auto stack_page = memory::vmm::kernel_space()->alloc(
        AP_STACK_SIZE, memory::vmm::AccessFlags::Read | memory::vmm::AccessFlags::Write |
                           memory::vmm::AccessFlags::Stack | memory::vmm::AccessFlags::Populate);

    if (!stack_page) {
      utils::logger::fatal("Failed to allocate stack page for AP {}!\n", cpu->processor_id);
    }

    cpu->extra_argument = stack_page->value() + AP_STACK_SIZE;
    __atomic_store_n(&cpu->goto_address, reinterpret_cast<void (*)(limine_mp_info *)>(ap_trampoline), __ATOMIC_SEQ_CST);

    utils::logger::info("Waking up AP {}...\n", cpu->processor_id);
  }
}
} // namespace kernel::hw::percpu