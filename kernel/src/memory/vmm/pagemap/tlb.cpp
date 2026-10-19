#include "memory/vmm/pagemap/tlb.hpp"

#include "hal/patch.h"
#include "hal/smp/percpu.hpp"
#include "memory/vmm/pagemap/active_pcid.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm::tlb {
namespace {
namespace flush {
namespace pcid {
void context(const std::uint16_t pcid) noexcept {
  hw::InvpcidDescriptor desc;
  if (pcid == 0) {
    hw::invpcid(hw::InvpcidType::AllContextsRetainGlobals, desc);
  } else {
    desc.set_pcid(pcid);
    hw::invpcid(hw::InvpcidType::SingleContext, desc);
  }
}

void single(const std::uint16_t pcid, const VirtualAddress addr) noexcept {
  utils::logger::info("address = 0x{:x}\n", addr);
  hw::InvpcidDescriptor desc;
  desc.set_pcid(pcid);
  desc.set_address(addr);
  hw::invpcid(hw::InvpcidType::IndividualAddress, desc);
}

void context_legacy(const std::uint16_t target_pcid) noexcept {
  const hw::CR3 cr3 = hw::read::cr3();

  if (target_pcid == cr3.get_pcid()) {
    const PhysicalAddress pfn{cr3.extract_address()};
    hw::write::cr3(hw::CR3::build_pcid(pfn, target_pcid, false));
  } else {
    const hw::CR4 cr4 = hw::read::cr4();
    hw::write::cr4(cr4.with_pcide(false));
    hw::write::cr4(cr4.with_pcide(true));
  }
}

void single_legacy(const std::uint16_t target_pcid, const VirtualAddress addr) noexcept {
  const hw::CR3 cr3 = hw::read::cr3();

  if (target_pcid == cr3.get_pcid())
    hw::invlpg(addr.value());
  else
    context_legacy(0);
}
}  // namespace pcid

namespace legacy {
void context(std::uint16_t) noexcept { hw::write::cr3(hw::read::cr3()); }
void single(std::uint16_t, const VirtualAddress addr) noexcept { hw::invlpg(addr.value()); }
}  // namespace legacy

DECLARE_STATIC_CALL(void, context, std::uint16_t);
DEFINE_STATIC_CALL(context, legacy::context);
REGISTER_STATIC_CALL_ALT_PRIO(context, pcid::context, 200, hw::Feature::INVPCID);
REGISTER_STATIC_CALL_ALT_PRIO(context, pcid::context_legacy, 100, hw::Feature::PCID);

DECLARE_STATIC_CALL(void, single, std::uint16_t, memory::VirtualAddress);
DEFINE_STATIC_CALL(single, legacy::single);
REGISTER_STATIC_CALL_ALT_PRIO(single, pcid::single_legacy, 100, hw::Feature::PCID);
REGISTER_STATIC_CALL_ALT_PRIO(single, pcid::single, 200, hw::Feature::INVPCID, hw::Feature::PCID);

void remote_shootdown_ipi(PageMap* pmap, const VirtualAddress virt, const std::size_t size_bytes,
                          [[maybe_unused]] const bool is_huge,
                          const std::uint32_t my_cpu) noexcept {
  std::uint32_t target_count = 0;

  pmap->cpu_tracker().for_each_active_cpu([&](const std::uint32_t cpu_id, std::uint16_t) noexcept {
    if (cpu_id != my_cpu) target_count++;
  });

  if (target_count == 0) [[likely]]
    return;

  const ShootdownRequest req{
      .pmap = pmap,
      .base_virt = virt,
      .size_bytes = size_bytes,
      .pending_count = target_count,
  };

  pmap->cpu_tracker().for_each_active_cpu([&](const std::uint32_t cpu_id, std::uint16_t) noexcept {
    if (cpu_id != my_cpu) {
      // hw::smp::ipi_send(cpu_id, hw::smp::IpiVector::TlbShootdown, &req);
    }
  });

  while (req.pending_count.load(std::memory_order_acquire) > 0) hw::pause();
}

void remote_shootdown_invlpgb(PageMap* pmap, const VirtualAddress virt,
                              const std::size_t size_bytes, const bool is_huge,
                              const std::uint32_t my_cpu) noexcept {
  // Fall back to software IPI shootdown for full context invalidations
  if (size_bytes == std::numeric_limits<std::uint64_t>::max()) [[unlikely]] {
    remote_shootdown_ipi(pmap, virt, size_bytes, is_huge, my_cpu);
    return;
  }

  bool sent_broadcast = false;
  std::uint64_t broadcasted_pcids[64] = {0};

  pmap->cpu_tracker().for_each_active_cpu(
      [&](const std::uint32_t cpu, const std::uint16_t remote_pcid) noexcept {
        if (cpu != my_cpu) {
          const std::size_t word = remote_pcid / 64;
          const std::uint64_t bit = 1ULL << (remote_pcid % 64);

          if ((broadcasted_pcids[word] & bit) == 0) {
            ShootdownCoordinator::queue_invlpgb(virt, size_bytes, remote_pcid, is_huge);
            broadcasted_pcids[word] |= bit;
            sent_broadcast = true;
          }
        }
      });

  if (sent_broadcast) {
    hw::tlbsync();
  }
}

DECLARE_STATIC_CALL(void, remote_shootdown, PageMap*, VirtualAddress, std::size_t, bool,
                    std::uint32_t);
DEFINE_STATIC_CALL(remote_shootdown, remote_shootdown_ipi);
REGISTER_STATIC_CALL_ALT_PRIO(remote_shootdown, remote_shootdown_invlpgb, 100, hw::Feature::INVLPGB,
                              !hw::Feature::HYPERVISOR);
}  // namespace flush

void load_map_pcid(const PhysicalAddress addr, std::uint16_t pcid, bool no_flush) noexcept {
  hw::write::cr3(hw::CR3::build_pcid(addr, pcid, no_flush));
}

void load_map_legacy(const PhysicalAddress addr, std::uint16_t, bool) noexcept {
  hw::write::cr3(hw::CR3::build_legacy(addr));
}

DECLARE_STATIC_CALL(void, load_cr3, PhysicalAddress, std::uint16_t, bool = false);
DEFINE_STATIC_CALL(load_cr3, load_map_legacy);
REGISTER_STATIC_CALL_ALT(load_cr3, load_map_pcid, hw::Feature::PCID);
}  // namespace

void PcidManager::rollover_epoch() noexcept {
  const std::uint32_t my_cpu = hw::percpu::id();

  for (auto& [pmap, pcid, epoch] : m_table) {
    if (epoch == m_curr_epoch && pmap != nullptr) {
      pmap->cpu_tracker().unmark(my_cpu);
    }
  }

  m_curr_epoch++;
  m_next_pcid = 1;
  flush::context(0);
}

void PcidManager::load(PageMap* pagemap) noexcept {
  std::size_t idx = hash(pagemap);
  const std::uint32_t my_cpu = hw::percpu::id();

  while (true) {
    auto& [pmap, pcid, epoch] = m_table[idx];

    // Fast-path: Already loaded in this epoch
    if (epoch == m_curr_epoch && pmap == pagemap) [[likely]] {
      load_cr3(pmap->root_phys(), pcid, true);
      return;
    }

    // Slot is free or stale
    if (epoch != m_curr_epoch) [[unlikely]] {
      if (m_next_pcid > MAX_PCID) [[unlikely]] {
        rollover_epoch();
        idx = hash(pagemap);
        continue;
      }

      if (pmap != nullptr) {
        pmap->cpu_tracker().unmark(my_cpu);
      }

      const std::uint16_t assigned_pcid = m_next_pcid++;
      pmap = pagemap;
      pcid = assigned_pcid;
      epoch = m_curr_epoch;

      hw::ActivePcidTracker& tracker = pagemap->cpu_tracker();
      tracker.mark(my_cpu, assigned_pcid);
      load_cr3(pagemap->root_phys(), assigned_pcid, false);
      return;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

std::uint16_t PcidManager::get_active_pcid(const PageMap* pagemap) const noexcept {
  std::size_t idx = hash(pagemap);

  while (true) {
    const auto& [pmap, pcid, epoch] = m_table[idx];
    if (epoch != m_curr_epoch) {
      return 0;
    }

    if (pmap == pagemap) {
      return pcid;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

void PcidManager::invalidate_active_pcid(const PageMap* pagemap) noexcept {
  std::size_t idx = hash(pagemap);
  const std::uint32_t my_cpu = hw::percpu::id();

  while (true) {
    auto& [pmap, _, epoch] = m_table[idx];
    if (epoch != m_curr_epoch) {
      return;
    }

    if (pmap == pagemap) {
      pmap->cpu_tracker().unmark(my_cpu);
      pmap = nullptr;
      return;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

void ShootdownCoordinator::local_invalidate(const PageMap* pmap, const VirtualAddress virt,
                                            const std::size_t size, const bool is_huge) noexcept {
  const PcidManager& local_tlb = hw::percpu::pcid_manager();
  const std::uint16_t pcid = local_tlb.get_active_pcid(pmap);

  if (size == std::numeric_limits<std::uint64_t>::max()) {
    flush::context(pcid);
  } else {
    const std::size_t step = is_huge ? PAGE_SIZE_2MB : PAGE_SIZE;

    for (std::size_t offset = 0; offset < size; offset += step) {
      flush::single(pcid, virt + offset);
    }
  }
}

void ShootdownCoordinator::queue_invlpgb(const VirtualAddress virt, const std::size_t size_bytes,
                                         const std::uint16_t target_pcid,
                                         const bool is_huge) noexcept {
  hw::InvlpgbCount ecx{0};

  const std::uintptr_t virt_addr = virt.value();
  auto rax = hw::InvlpgbAddress{}
                 .with_va_valid()     // Flush only the specific virtual address
                 .with_pcid_valid();  // Flush specific pcid value
  rax.set_include_global(virt_addr >= DirectMap::s_hhdm_base);

  hw::InvlpgbContext edx{0};
  edx.set_pcid(target_pcid);

  ecx.set_large_page_stride(true);

  const std::size_t page_size = is_huge ? PAGE_SIZE_2MB : PAGE_SIZE;
  std::size_t pages = utils::maths::div_round_up(size_bytes, page_size);
  std::uintptr_t curr_virt = virt_addr;

  while (pages > 0) {
    const std::uint32_t chunk = std::min<std::size_t>(pages, 4096);

    rax.set_virt_addr(curr_virt >> 12);
    ecx.set_extra_count(chunk - 1);
    hw::invlpgb(rax, ecx, edx);

    pages -= chunk;
    curr_virt += static_cast<std::uintptr_t>(chunk) * page_size;
  }
}

void ShootdownCoordinator::broadcast(PageMap* pmap, const VirtualAddress virt,
                                     const std::size_t size_bytes, const bool is_huge) noexcept {
  const std::uint32_t my_cpu = hw::percpu::id();

  local_invalidate(pmap, virt, size_bytes, is_huge);
  flush::remote_shootdown(pmap, virt, size_bytes, is_huge, my_cpu);
}

void ShootdownCoordinator::handle_ipi(ShootdownRequest* req) noexcept {
  const bool is_huge = req->size_bytes == PAGE_SIZE_2MB || req->size_bytes == PAGE_SIZE_1GB;
  local_invalidate(req->pmap, req->base_virt, req->size_bytes, is_huge);
  req->pending_count.fetch_sub(1, std::memory_order_release);
}
}  // namespace kernel::memory::vmm::tlb