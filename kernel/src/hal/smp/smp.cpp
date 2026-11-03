#include "hal/smp/smp.hpp"

#include <algorithm>
#include <chrono.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "drivers/acpi.hpp"
#include "drivers/clock.hpp"
#include "hal/apic/ioapic.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"
#include "utils/maths.hpp"

namespace kernel::hw::smp {
namespace detail {
CpuEntry* g_cpus = nullptr;
std::atomic<std::uint64_t>* g_online_mask = nullptr;
std::uint32_t g_mask_words = 0;
std::atomic<std::uint32_t> g_online_count{0};
}  // namespace detail

namespace {
using namespace klib::literals;
using detail::g_cpus;
using detail::g_online_count;
using detail::g_online_mask;

constexpr std::size_t AP_STACK_SIZE = KSTACK_SIZE;
constexpr std::size_t EXCEPTION_STACK_SIZE = 2 * memory::PAGE_SIZE;

// Every AP is started at the same time, so this bounds the whole bring-up rather than each CPU.
constexpr klib::Duration AP_BOOT_TIMEOUT = 5_s;

// How long a panicking CPU waits for the others to confirm that they have stopped.
constexpr klib::Duration STOP_TIMEOUT = 100_ms;

std::uint32_t g_possible = 1;  // CPUs we use, capped at MAX_CPUS
std::uint32_t g_reported = 1;  // CPUs the boot loader listed
bool g_cluster_dest = false;

memory::VirtualAddress g_lapic_mmio{};
std::uint64_t g_kernel_cr3 = 0;

// Placeholder until there is a scheduler: nobody ever has anything to run.
std::uint32_t g_runqueue = 0;

// What the APs run once online, instead of going straight to the idle loop.
void (*g_ap_main)() noexcept = nullptr;

std::atomic<std::uint32_t> g_stopper{INVALID_CPU};
std::atomic<std::uint32_t> g_stopped{0};

// Spins until `done()` or until @p timeout has passed. Without a calibrated clock the timeout
// becomes a generous spin count instead, so a broken clock delays the boot but does not hang it.
template <typename Pred>
bool spin_until(Pred&& done, const klib::Duration timeout) noexcept {
  if (drivers::SystemClock::scaler().freq_hz == 0) [[unlikely]] {
    for (std::uint64_t spins = 0; spins < (1ull << 30); ++spins) {
      if (done()) return true;
      pause();
    }

    return done();
  }

  const klib::TimePoint deadline = drivers::SystemClock::now() + timeout;
  while (!done()) {
    if (drivers::SystemClock::now() > deadline) return done();
    cpu_relax();
  }

  return true;
}

// Calls `fn(id, info)` for every AP we use, with the dense ID it gets.
template <typename Fn>
void for_each_ap(const limine_mp_response* resp, Fn&& fn) noexcept {
  std::uint32_t id = 1;
  for (std::uint64_t i = 0; i < resp->cpu_count && id < g_possible; ++i) {
    limine_mp_info* info = resp->cpus[i];
    if (info->lapic_id != resp->bsp_lapic_id) fn(id++, info);
  }
}

[[nodiscard]] memory::VirtualAddress alloc_stack(const std::size_t size,
                                                 const std::uint32_t cpu) noexcept {
  using memory::vmm::AccessFlags;
  const auto base = memory::vmm::kernel_space()->alloc(
      size, AccessFlags::Read | AccessFlags::Write | AccessFlags::Stack | AccessFlags::Populate);
  if (!base) {
    utils::logger::fatal("SMP: out of memory for the stacks of CPU {}\n", cpu);
  }

  return *base + size;
}

// The registry: one entry per CPU, plus the online bitmap.
void build_registry(const limine_mp_response* resp) noexcept {
  const std::uint32_t words = utils::maths::div_round_up(g_possible, 64u);

  auto* cpus = static_cast<CpuEntry*>(
      memory::heap::kmalloc(sizeof(CpuEntry) * g_possible, alignof(CpuEntry)));
  auto* mask = static_cast<std::atomic<std::uint64_t>*>(memory::heap::kmalloc(
      sizeof(std::atomic<std::uint64_t>) * words, std::hardware_destructive_interference_size));
  if (!cpus || !mask) {
    utils::logger::fatal("SMP: out of memory for the registry of {} CPUs\n", g_possible);
  }

  std::uninitialized_default_construct_n(cpus, g_possible);
  std::uninitialized_value_construct_n(mask, words);

  cpus[0].area = percpu::self();
  if (resp) {
    cpus[0].apic_id = resp->bsp_lapic_id;
    for (std::uint64_t i = 0; i < resp->cpu_count; ++i) {
      if (resp->cpus[i]->lapic_id == resp->bsp_lapic_id) {
        cpus[0].acpi_uid = resp->cpus[i]->processor_id;
      }
    }

    for_each_ap(resp, [cpus](const std::uint32_t id, const limine_mp_info* info) {
      cpus[id].apic_id = info->lapic_id;
      cpus[id].acpi_uid = info->processor_id;
    });
  }

  g_online_mask = mask;
  detail::g_mask_words = words;
  g_cpus = cpus;
}

struct ApicAffinity {
  std::uint32_t apic_id;
  std::uint32_t domain;
};

[[nodiscard]] std::optional<ApicAffinity> affinity_of(const acpi_entry_hdr* hdr) noexcept {
  if (hdr->type == ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY) {
    const auto* p = drivers::acpi::entry_as<acpi_srat_processor_affinity>(hdr);
    if (!p || (p->flags & ACPI_SRAT_PROCESSOR_ENABLED) == 0) {
      return std::nullopt;
    }

    // The domain is split: bits 7:0 up front, 31:8 further down (zero before SRAT revision 2).
    const std::uint32_t domain = p->proximity_domain_low |
                                 (std::uint32_t{p->proximity_domain_high[0]} << 8) |
                                 (std::uint32_t{p->proximity_domain_high[1]} << 16) |
                                 (std::uint32_t{p->proximity_domain_high[2]} << 24);
    return ApicAffinity{p->id, domain};
  }

  if (hdr->type == ACPI_SRAT_ENTRY_TYPE_X2APIC_AFFINITY) {
    const auto* p = drivers::acpi::entry_as<acpi_srat_x2apic_affinity>(hdr);
    if (!p || (p->flags & ACPI_SRAT_PROCESSOR_ENABLED) == 0) {
      return std::nullopt;
    }

    return ApicAffinity{p->id, p->proximity_domain};
  }

  return std::nullopt;
}

// Gives every CPU the PMM node of its SRAT proximity domain. CPUs in a domain without memory of
// its own, or missing from the SRAT, stay on node 0; the PMM's fallback lists take it from there.
void assign_numa_nodes() noexcept {
  using namespace drivers::acpi;

  auto srat_res = Table::find<"SRAT">();
  if (!srat_res) {
    return;
  }

  const Table srat = std::move(*srat_res);
  const auto payload = srat.payload<acpi_srat>();
  if (!payload) {
    return;
  }

  std::size_t count = 0;
  for (const acpi_entry_hdr* hdr : *payload) {
    count += affinity_of(hdr).has_value();
  }

  if (count == 0) {
    return;
  }

  auto* map = static_cast<ApicAffinity*>(memory::heap::kmalloc(sizeof(ApicAffinity) * count));
  if (!map) {
    utils::logger::warn("SMP: out of memory for the CPU affinity table, assuming one node\n");
    return;
  }

  std::size_t filled = 0;
  for (const acpi_entry_hdr* hdr : *payload) {
    if (const auto affinity = affinity_of(hdr)) {
      map[filled++] = *affinity;
    }
  }

  // Sorted once, so the lookups below stay O(n log n) even on very large machines.
  const std::span table{map, filled};
  std::ranges::sort(table, {}, &ApicAffinity::apic_id);

  const memory::pmm::TopologyParser& topology = memory::pmm::g_topology;
  std::uint32_t memoryless = 0;

  for (std::uint32_t cpu = 0; cpu < g_possible; ++cpu) {
    CpuEntry& e = g_cpus[cpu];
    const auto it = std::ranges::lower_bound(table, e.apic_id, {}, &ApicAffinity::apic_id);
    if (it == table.end() || it->apic_id != e.apic_id) {
      continue;
    }

    const std::uint32_t node = topology.get_internal_node_id(it->domain);
    if (node >= topology.active_nodes()) {
      ++memoryless;
      continue;
    }

    e.numa_node = node;
  }

  if (memoryless != 0) {
    utils::logger::warn("SMP: {} CPU(s) sit in NUMA domains without memory, using node 0\n",
                        memoryless);
  }

  memory::heap::kfree(map);
}

// The MADT says which LINT pin carries NMIs (front-panel NMI buttons, platform watchdogs and
// the like), either for one processor UID or for all of them.
void assign_nmi_sources() noexcept {
  using namespace drivers::acpi;

  auto madt_res = Table::find<ACPI_MADT_SIGNATURE>();
  if (!madt_res) {
    return;
  }

  const Table madt = std::move(*madt_res);
  const auto payload = madt.payload<acpi_madt>();
  if (!payload) {
    return;
  }

  // Firmware often lists one entry per processor, so look UIDs up in a sorted index rather than
  // scanning every CPU for every entry.
  struct UidIndex {
    std::uint32_t uid;
    std::uint32_t cpu;
  };

  auto* index = static_cast<UidIndex*>(memory::heap::kmalloc(sizeof(UidIndex) * g_possible));
  if (!index) {
    utils::logger::warn("SMP: out of memory for the NMI source table, LINT NMIs stay masked\n");
    return;
  }

  for (std::uint32_t cpu = 0; cpu < g_possible; ++cpu) {
    index[cpu] = {g_cpus[cpu].acpi_uid, cpu};
  }

  const std::span uids{index, g_possible};
  std::ranges::sort(uids, {}, &UidIndex::uid);

  const auto apply = [](CpuEntry& e, const std::uint8_t lint, const std::uint16_t flags) {
    e.nmi_lints |= static_cast<std::uint8_t>(1u << lint);
    if ((flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW) {
      e.nmi_active_low |= static_cast<std::uint8_t>(1u << lint);
    }
  };

  for (const acpi_entry_hdr* hdr : *payload) {
    std::uint32_t uid = 0;
    std::uint16_t flags = 0;
    std::uint8_t lint = 0;
    bool everyone = false;

    if (hdr->type == ACPI_MADT_ENTRY_TYPE_LAPIC_NMI) {
      const auto* nmi = entry_as<acpi_madt_lapic_nmi>(hdr);
      if (!nmi) {
        continue;
      }
      uid = nmi->uid;
      flags = nmi->flags;
      lint = nmi->lint;
      everyone = nmi->uid == 0xFF;
    } else if (hdr->type == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI) {
      const auto* nmi = entry_as<acpi_madt_x2apic_nmi>(hdr);
      if (!nmi) {
        continue;
      }
      uid = nmi->uid;
      flags = nmi->flags;
      lint = nmi->lint;
      everyone = nmi->uid == 0xFFFFFFFF;
    } else {
      continue;
    }

    if (lint > 1) {
      continue;
    }

    if (everyone) {
      for (std::uint32_t cpu = 0; cpu < g_possible; ++cpu) {
        apply(g_cpus[cpu], lint, flags);
      }
      continue;
    }

    const auto it = std::ranges::lower_bound(uids, uid, {}, &UidIndex::uid);
    if (it != uids.end() && it->uid == uid) {
      apply(g_cpus[it->cpu], lint, flags);
    }
  }

  memory::heap::kfree(index);
}

// FADT flag that firmware sets when logical destination mode must not be used. uACPI keeps a
// full-size copy of the FADT, so short (old) FADTs need no special care here.
[[nodiscard]] bool fadt_forces_physical_destinations() noexcept {
  acpi_fadt* fadt = nullptr;
  return uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt &&
         (fadt->flags & ACPI_FORCE_APIC_PHYS_DEST_MODE) != 0;
}

// Stacks of a CPU that the BSP sets up before starting it: the TSS points at them.
void prepare_stacks(PerCpu* cpu, const std::uint32_t id) noexcept {
  cpu->gdt_table.set_kernel_stack(cpu->stack_top)
      .set_interrupt_stack(gdt::DOUBLE_FAULT_IST, alloc_stack(EXCEPTION_STACK_SIZE, id))
      .set_interrupt_stack(gdt::NMI_IST, alloc_stack(EXCEPTION_STACK_SIZE, id))
      .set_interrupt_stack(gdt::MACHINE_CHECK_IST, alloc_stack(EXCEPTION_STACK_SIZE, id));
}

// Interrupt delivery and the local APIC. Runs on the CPU being brought up.
void bring_up_local(PerCpu* cpu) noexcept {
  CpuEntry& e = g_cpus[cpu->topology.get_cpu_id()];

  interrupts::initialize(cpu->stack_top, cpu->gdt_table.interrupt_stack(gdt::NMI_IST));

  cpu->lapic.initialize(g_lapic_mmio, true);
  if (apic::io_apic_manager.supports_directed_eoi()) {
    cpu->lapic.enable_directed_eoi(true);
  }

  for (std::uint8_t lint = 0; lint < 2; ++lint) {
    if ((e.nmi_lints >> lint) & 1) {
      cpu->lapic.configure_nmi(
          lint == 0 ? apic::Reg::LvtLint0 : apic::Reg::LvtLint1,
          ((e.nmi_active_low >> lint) & 1) ? apic::PinPolarity::Low : apic::PinPolarity::High);
    }
  }

  if (cpu->lapic.id() != e.apic_id) [[unlikely]] {
    utils::logger::warn("SMP: CPU {} was listed with APIC ID {} but has {}\n",
                        cpu->topology.get_cpu_id(), e.apic_id, cpu->lapic.id());
  }

  e.apic_id = cpu->lapic.id();
  e.logical_dest = cpu->lapic.logical_id();
}

// Makes the calling CPU a target for IPIs. Interrupts must still be disabled.
[[nodiscard]] bool go_online(const std::uint32_t cpu, CpuState from) noexcept {
  if (!g_cpus[cpu].state.compare_exchange_strong(from, CpuState::Online,
                                                 std::memory_order_seq_cst)) {
    return false;
  }

  g_online_mask[cpu / 64].fetch_or(1ull << (cpu % 64), std::memory_order_seq_cst);
  g_online_count.fetch_add(1, std::memory_order_seq_cst);

  // Pairs with the fence in ipi::call_many(). Whoever changed shared state (page tables, say)
  // before picking its targets either finds us online, or its changes are visible to us from
  // here on. The TLB may still hold entries from before, so drop them all.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  memory::vmm::tlb::ShootdownCoordinator::cpu_online();
  return true;
}

[[noreturn]] void park_stopped(const std::uint32_t cpu) noexcept {
  irq::disable();

  // NMIs can still wake the halted CPU and bring it back here; count it once.
  if (g_cpus[cpu].state.exchange(CpuState::Stopped, std::memory_order_acq_rel) !=
      CpuState::Stopped) {
    g_online_mask[cpu / 64].fetch_and(~(1ull << (cpu % 64)), std::memory_order_relaxed);
    g_stopped.fetch_add(1, std::memory_order_release);
  }

  dead_loop();
}

[[noreturn]] void ap_main(PerCpu* cpu) noexcept {
  initialize_secondary();
  memory::vmm::initialize_hw();
  percpu::install(cpu);

  // The entry stub loaded the kernel map by hand. Load it again through the PCID manager so this
  // CPU's bookkeeping matches what CR3 really holds.
  percpu::pcid_manager().load(&memory::vmm::kernel_space()->pagemap());
  cpu->info = profile_manager.register_cpu();

  bring_up_local(cpu);

  const std::uint32_t id = percpu::id();
  if (!go_online(id, CpuState::Booting)) {
    // The BSP has given up on us. Joining late would surprise everyone who already counted the
    // CPUs, so stay out of the way for good.
    dead_loop();
  }

  irq::enable();

  utils::logger::info("CPU {} online (APIC ID {}, node {})\n", id, g_cpus[id].apic_id,
                      g_cpus[id].numa_node);

  if (g_ap_main != nullptr) {
    g_ap_main();
  }

  cpu_idle_loop(&g_runqueue);
}

// Where Limine sends each AP: still on the boot loader's page tables and stack, interrupts off.
void ap_entry(limine_mp_info* info) noexcept {
  auto* cpu = reinterpret_cast<PerCpu*>(info->extra_argument);

  // The per-CPU area is only mapped in the kernel's page tables.
  write::cr3(CR3{g_kernel_cr3});
  execute_on_new_stack(cpu->stack_top.value(), cpu,
                       [](void* arg) { ap_main(static_cast<PerCpu*>(arg)); });
}

std::uint32_t start_aps(const limine_mp_response* resp) noexcept {
  if (!resp) {
    return 0;
  }

  g_kernel_cr3 = CR3::build_legacy(memory::vmm::kernel_space()->pagemap().root_phys()).raw();

  std::uint32_t started = 0;
  for_each_ap(resp, [&started](const std::uint32_t id, limine_mp_info* info) {
    using memory::vmm::AccessFlags;
    const auto area = memory::vmm::kernel_space()->alloc(
        sizeof(PerCpu), AccessFlags::Read | AccessFlags::Write | AccessFlags::Populate);
    if (!area) {
      utils::logger::fatal("SMP: out of memory for the per-CPU area of CPU {}\n", id);
    }

    CpuEntry& e = g_cpus[id];
    PerCpu* cpu = new (area->as<PerCpu>()) PerCpu();
    cpu->topology = CpuTopology{}.with_cpu_id(id).with_numa_node(e.numa_node);
    cpu->stack_top = alloc_stack(AP_STACK_SIZE, id);
    prepare_stacks(cpu, id);

    e.area = cpu;
    e.state.store(CpuState::Booting, std::memory_order_relaxed);

    // The AP starts as soon as it sees its entry point, so publish everything above first.
    info->extra_argument = reinterpret_cast<std::uint64_t>(cpu);
    __atomic_store_n(&info->goto_address, &ap_entry, __ATOMIC_RELEASE);
    ++started;
  });

  return started;
}

void wait_for_aps(const std::uint32_t started) noexcept {
  const bool all_up = spin_until(
      [started] { return g_online_count.load(std::memory_order_acquire) == started + 1; },
      AP_BOOT_TIMEOUT);

  if (all_up) {
    return;
  }

  for (std::uint32_t cpu = 1; cpu < g_possible; ++cpu) {
    CpuState expected = CpuState::Booting;
    if (g_cpus[cpu].state.compare_exchange_strong(expected, CpuState::Failed)) {
      utils::logger::warn("SMP: CPU {} (APIC ID {}) did not come up, leaving it alone\n", cpu,
                          g_cpus[cpu].apic_id);
    }
  }
}
}  // namespace

bool all_online() noexcept { return g_online_count.load(std::memory_order_acquire) == g_reported; }

bool uses_cluster_destinations() noexcept { return g_cluster_dest; }

void set_ap_main(void (*const main)() noexcept) noexcept { g_ap_main = main; }

void initialize(const memory::VirtualAddress bsp_stack_top) noexcept {
  const limine_mp_response* resp = boot::mp_request.response;
  g_reported = resp && resp->cpu_count != 0 ? static_cast<std::uint32_t>(resp->cpu_count) : 1;
  g_possible = possible_cpus();

  if (g_reported > g_possible) [[unlikely]] {
    utils::logger::warn("SMP: firmware lists {} CPUs, only the first {} will be used\n", g_reported,
                        g_possible);
  }

  build_registry(resp);
  assign_numa_nodes();
  assign_nmi_sources();

  PerCpu* bsp = percpu::self();
  WRITE_PCP(topology, CpuTopology{}.with_cpu_id(0).with_numa_node(g_cpus[0].numa_node));
  bsp->stack_top = bsp_stack_top;
  prepare_stacks(bsp, 0);

  // Only xAPIC mode uses the MMIO window, but the mapping is cheap and keeps the code simple.
  const memory::PhysicalAddress lapic_phys{
      apic::BaseMsrSchema{read::msr(apic::IA32_APIC_BASE)}.get_base_address()
      << memory::PAGE_SHIFT_4KB};
  const auto lapic_mmio = memory::vmm::kernel_space()->map_mmio(lapic_phys, memory::PAGE_SIZE);
  if (!lapic_mmio) {
    utils::logger::fatal("SMP: failed to map the local APIC: {}\n",
                         std::to_underlying(lapic_mmio.error()));
  }
  g_lapic_mmio = *lapic_mmio;

  apic::io_apic_manager.initialize();
  bring_up_local(bsp);

  g_cluster_dest =
      bsp->lapic.mode() == apic::ApicMode::x2APIC && !fadt_forces_physical_destinations();

  ipi::initialize();
  if (!go_online(0, CpuState::Offline)) [[unlikely]] {
    utils::logger::fatal("SMP: the BSP is already online?\n");
  }
  irq::enable();

  wait_for_aps(start_aps(resp));

  const char* ipi_mode = "xAPIC physical";
  if (g_cluster_dest) {
    ipi_mode = "x2APIC cluster";
  } else if (bsp->lapic.mode() == apic::ApicMode::x2APIC) {
    ipi_mode = "x2APIC physical";
  }

  utils::logger::info("SMP: {} of {} CPUs online, {} NUMA node(s), {} IPIs\n", online_cpus(),
                      g_possible, memory::pmm::g_topology.active_nodes(), ipi_mode);
}

void stop_other_cpus() noexcept {
  // Nothing else runs before the registry exists, and GS may not even be set up yet.
  if (!g_cpus) {
    return;
  }

  const std::uint32_t me = percpu::id();
  std::uint32_t expected = INVALID_CPU;
  if (!g_stopper.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {
    if (expected == me) {
      return;  // a second panic while stopping
    }

    // Someone else is stopping the world. Its NMI is on the way; save it the trouble.
    park_stopped(me);
  }

  std::uint32_t others = 0;
  for_each_online([&others, me](const std::uint32_t cpu) { others += cpu != me; });
  if (others == 0) {
    return;
  }

  const apic::Lapic& lapic = percpu::lapic();
  if (all_online()) {
    lapic.send_ipi(apic::DestinationShorthand::AllExcludingSelf, 0, apic::DeliveryMode::NMI);
  } else {
    for_each_online([&lapic, me](const std::uint32_t cpu) {
      if (cpu != me) {
        lapic.send_ipi(g_cpus[cpu].apic_id, 0, apic::DeliveryMode::NMI);
      }
    });
  }

  if (!spin_until([others] { return g_stopped.load(std::memory_order_acquire) >= others; },
                  STOP_TIMEOUT)) {
    utils::logger::error("SMP: {} of {} CPUs did not stop\n",
                         others - g_stopped.load(std::memory_order_relaxed), others);
  }
}

bool handle_stop_nmi() noexcept {
  const std::uint32_t stopper = g_stopper.load(std::memory_order_acquire);
  if (stopper == INVALID_CPU || !g_cpus) {
    return false;
  }

  const std::uint32_t me = percpu::id();
  if (stopper == me) {
    return false;
  }

  park_stopped(me);
}
}  // namespace kernel::hw::smp

namespace kernel::hw {
void panic_halt() noexcept {
  irq::disable();
  smp::stop_other_cpus();
  dead_loop();
}
}  // namespace kernel::hw
