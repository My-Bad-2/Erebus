#include "memory/vmm/tlb.hpp"
#include "../../../include/memory/vmm/active_pcid.hpp"
#include "hal/percpu.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm::tlb {
namespace {
namespace flush {
hw::patcher::StaticKeyDef invpcid_supported{false};
hw::patcher::StaticKeyDef pcid_supported{false};
hw::patcher::StaticKeyDef invlpgb_supported{false};

namespace pcid {
void context(const std::uint16_t pcid) noexcept {
  if (pcid == 0) {
    hw::invpcid(hw::InvpcidType::AllContextsRetainGlobals, {.pcid = 0, .reserved = 0, .address = 0});
  } else {
    hw::invpcid(hw::InvpcidType::SingleContext, {.pcid = pcid, .reserved = 0, .address = 0});
  }
}

void single(const std::uint16_t pcid, const std::uintptr_t addr) noexcept {
  hw::invpcid(hw::InvpcidType::IndividualAddress, {pcid, 0, addr});
}

void context_legacy(const std::uint16_t target_pcid) noexcept {
  const hw::CR3 cr3 = hw::read::cr3();

  if (target_pcid == cr3.pcid().get<"pcid">()) {
    const PhysicalAddress pfn{cr3.extract_address()};
    hw::write::cr3(hw::CR3::build_pcid(pfn, target_pcid, false));
  } else {
    hw::CR4Schema cr4 = hw::read::cr4();

    cr4.set_mut<"pcide">(0);
    hw::write::cr4(cr4);

    cr4.set_mut<"pcide">(1);
    hw::write::cr4(cr4);
  }
}

void single_legacy(const std::uint16_t target_pcid, const std::uintptr_t addr) noexcept {
  const hw::CR3 cr3 = hw::read::cr3();

  if (target_pcid == cr3.pcid().get<"pcid">()) {
    hw::invlpg(addr);
  } else {
    context_legacy(0);
  }
}
} // namespace pcid

namespace legacy {
void context() noexcept { hw::write::cr3(hw::read::cr3()); }
void single(const std::uintptr_t addr) noexcept { hw::invlpg(addr); }
} // namespace legacy

void context(const std::uint16_t pcid) {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(invpcid_supported)) {
    pcid::context(pcid);
  } else if (EREBUS_STATIC_BRANCH_UNLIKELY(pcid_supported)) {
    pcid::context_legacy(pcid); // pcid is supported but not invpcid
  } else {
    legacy::context();
  }
}

void single(const std::uint16_t pcid, std::uintptr_t addr) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(invpcid_supported)) {
    pcid::single(pcid, addr);
  } else if (EREBUS_STATIC_BRANCH_UNLIKELY(pcid_supported)) {
    pcid::single_legacy(pcid, addr);
  } else {
    legacy::single(addr);
  }
}
} // namespace flush

void load_cr3(const PhysicalAddress addr, const std::uint16_t pcid, const bool no_flush = false) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(flush::pcid_supported)) {
    hw::write::cr3(hw::CR3::build_pcid(addr, pcid, no_flush));
  } else {
    hw::write::cr3(hw::CR3::build_legacy(addr));
  }
}
} // namespace

void PcidManager::rollover_epoch() noexcept {
  const std::uint32_t my_cpu = hw::percpu::id();

  for (auto &[pmap, pcid, epoch] : m_table) {
    if (epoch == m_curr_epoch && pmap != nullptr) {
      pmap->cpu_tracker().unmark(my_cpu);
    }
  }

  m_curr_epoch++;
  m_next_pcid = 1;
  flush::context(0);
}

void PcidManager::load(PageMap *pagemap) noexcept {
  std::size_t idx = hash(pagemap);
  const std::uint32_t my_cpu = hw::percpu::id();

  while (true) {
    auto &[pmap, pcid, epoch] = m_table[idx];

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

      hw::ActivePcidTracker &tracker = pagemap->cpu_tracker();
      tracker.mark(my_cpu, assigned_pcid);
      load_cr3(pagemap->root_phys(), assigned_pcid, false);
      return;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

std::uint16_t PcidManager::get_active_pcid(const PageMap *pagemap) const noexcept {
  std::size_t idx = hash(pagemap);

  while (true) {
    const auto &[pmap, pcid, epoch] = m_table[idx];
    if (epoch != m_curr_epoch) {
      return 0;
    }

    if (pmap == pagemap) {
      return pcid;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

void PcidManager::invalidate_active_pcid(const PageMap *pagemap) noexcept {
  std::size_t idx = hash(pagemap);
  const std::uint32_t my_cpu = hw::percpu::id();

  while (true) {
    auto &[pmap, _, epoch] = m_table[idx];
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

void ShootdownCoordinator::local_invalidate(const PageMap *pmap, const VirtualAddress virt, const std::size_t size,
                                            const bool is_huge) noexcept {
  const PcidManager &local_tlb = hw::percpu::pcid_manager();
  const std::uint16_t pcid = local_tlb.get_active_pcid(pmap);

  if (size == std::numeric_limits<std::uint64_t>::max()) {
    flush::context(pcid);
  } else {
    const std::size_t step = is_huge ? PAGE_SIZE_2MB : PAGE_SIZE;
    const std::uintptr_t virt_addr = virt.value();

    for (std::size_t offset = 0; offset < size; offset += step) {
      flush::single(pcid, virt_addr + offset);
    }
  }
}

void ShootdownCoordinator::queue_invlpgb(const VirtualAddress virt, const std::size_t size_bytes,
                                         const std::uint16_t target_pcid, const bool is_huge) noexcept {
  hw::InvlpgbAddressFlagsSchema rax{0};
  hw::InvlpgbContextIdsSchema edx{0};
  hw::InvlpgbPageCountSchema ecx{0};

  const std::uintptr_t virt_addr = virt.value();

  rax.set_mut<"va_valid">(1);
  rax.set_mut<"pcid_valid">(1);

  if (virt_addr >= DirectMap::s_hhdm_base) {
    rax.set_mut<"include_global">(1);
  }

  edx.set_mut<"pcid">(target_pcid);
  if (is_huge) {
    ecx.set_mut<"large_page_stride">(1);
  }

  const std::size_t page_size = is_huge ? PAGE_SIZE_2MB : PAGE_SIZE;
  std::size_t pages = utils::maths::div_round_up(size_bytes, page_size);
  std::uintptr_t curr_virt = virt_addr;

  while (pages > 0) {
    const std::uint32_t chunk = std::min<std::size_t>(pages, 4096);

    rax.set_mut<"virt_addr">(curr_virt >> 12);
    ecx.set_mut<"extra_count">(chunk - 1);
    hw::invlpgb(rax, ecx, edx);

    pages -= chunk;
    curr_virt += static_cast<std::uintptr_t>(chunk) * page_size;
  }
}

void ShootdownCoordinator::broadcast(PageMap *pmap, const VirtualAddress virt, const std::size_t size_bytes,
                                     const bool is_huge) noexcept {
  const std::uint32_t my_cpu = hw::percpu::id();

  local_invalidate(pmap, virt, size_bytes, is_huge);

  if (EREBUS_STATIC_BRANCH_UNLIKELY(flush::invlpgb_supported)) {
    if (size_bytes != std::numeric_limits<std::uint64_t>::max()) {
      bool sent_broadcast = false;
      std::uint64_t broadcasted_pcids[64] = {0};

      pmap->cpu_tracker().for_each_active_cpu([&](const std::uint32_t cpu, const std::uint16_t remote_pcid) noexcept {
        if (cpu != my_cpu) {
          if ((broadcasted_pcids[remote_pcid / 64] & (1ull << (remote_pcid % 64))) == 0) {
            queue_invlpgb(virt, size_bytes, remote_pcid, is_huge);
            broadcasted_pcids[remote_pcid / 64] |= (1ull << (remote_pcid % 64));
            sent_broadcast = true;
          }
        }
      });

      if (sent_broadcast) {
        hw::tlbsync();
      }
      return;
    }
  }

  std::uint32_t target_count = 0;

  pmap->cpu_tracker().for_each_active_cpu([&](const std::uint32_t cpu_id, std::uint16_t) noexcept {
    if (cpu_id != my_cpu) {
      target_count++;
    }
  });

  if (target_count == 0) [[likely]] {
    return;
  }

  ShootdownRequest req{
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

  while (req.pending_count.load(std::memory_order_acquire) > 0) {
    hw::pause();
  }
}

void ShootdownCoordinator::handle_ipi(ShootdownRequest *req) noexcept {
  const bool is_huge = (req->size_bytes == PAGE_SIZE_2MB || req->size_bytes == PAGE_SIZE_1GB);
  local_invalidate(req->pmap, req->base_virt, req->size_bytes, is_huge);
  req->pending_count.fetch_sub(1, std::memory_order_release);
}

void initialize() noexcept {
  const hw::CpuInfo *cpu_info = hw::profile_manager.get_current();

  flush::invpcid_supported.enabled = cpu_info->has<hw::Feature::INVPCID>();
  flush::pcid_supported.enabled = cpu_info->has<hw::Feature::PCID>();
  flush::invlpgb_supported.enabled = cpu_info->has<hw::Feature::INVLPGB>();
}
} // namespace kernel::memory::vmm::tlb