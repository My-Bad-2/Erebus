#include "memory/vmm/pagemap/tlb.hpp"

#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "memory/vmm/pagemap/active_pcid.hpp"
#include "patch/static_call.hpp"

namespace kernel::memory::vmm::tlb {
// The static call trampolines are global symbols, so the declarations need external linkage.
// Every default below is correct on any CPU; the alternatives are only faster. That matters
// because the kernel runs on the defaults until StaticCallPatcher::apply_patches().
namespace detail {
DECLARE_STATIC_CALL(void, tlb_flush_everything);
DECLARE_STATIC_CALL(bool, tlb_flush_pcid, std::uint16_t);
DECLARE_STATIC_CALL(bool, tlb_flush_pcid_page, std::uint16_t, VirtualAddress);
DECLARE_STATIC_CALL(void, tlb_remote_shootdown, PageMap*, VirtualAddress, std::size_t, PageSize,
                    std::uint32_t);
}  // namespace detail

namespace {
// Past this many pages one full flush is cheaper than individual invalidations.
constexpr std::size_t SINGLE_PAGE_FLUSH_CEILING = 32;

[[nodiscard]] constexpr std::size_t stride_bytes(const PageSize stride) noexcept {
  return PAGE_SIZE << ((std::to_underlying(stride) - 1) * 9);
}

// Drops every TLB entry of every PCID, global ones included.
void flush_everything_pge() noexcept {
  const hw::CR4 cr4 = hw::read::cr4();
  if (cr4.get_pge()) {
    hw::write::cr4(cr4.with_pge(false));
    hw::write::cr4(cr4);
  } else {
    // No global pages to worry about. (PCID without PGE does not exist in practice.)
    hw::write::cr3(hw::read::cr3());
  }
}

void flush_everything_invpcid() noexcept {
  hw::invpcid(hw::InvpcidType::AllContexts, hw::InvpcidDescriptor{});
}

// Flushing a PCID that is not loaded needs INVPCID. Without it the caller retires the PCID.
bool flush_pcid_unsupported(std::uint16_t) noexcept { return false; }

bool flush_pcid_invpcid(const std::uint16_t pcid) noexcept {
  hw::InvpcidDescriptor desc;
  desc.set_pcid(pcid);
  hw::invpcid(hw::InvpcidType::SingleContext, desc);
  return true;
}

bool flush_pcid_page_unsupported(std::uint16_t, VirtualAddress) noexcept { return false; }

bool flush_pcid_page_invpcid(const std::uint16_t pcid, const VirtualAddress addr) noexcept {
  hw::InvpcidDescriptor desc;
  desc.set_pcid(pcid);
  desc.set_address(addr);
  hw::invpcid(hw::InvpcidType::IndividualAddress, desc);
  return true;
}

// Kernel translations can be cached anywhere; user ones only where the map was loaded.
bool is_shootdown_target(const std::uint32_t cpu, const void* ctx) noexcept {
  const auto* req = static_cast<const ShootdownRequest*>(ctx);
  return req->pmap->is_kernel() || req->base_virt.is_upper_half() ||
         req->pmap->cpu_tracker().test(cpu);
}

void run_shootdown(void* arg) noexcept {
  const auto* req = static_cast<const ShootdownRequest*>(arg);
  ShootdownCoordinator::local_invalidate(req->pmap, req->base_virt, req->size_bytes, req->stride);
}

void remote_shootdown_ipi(PageMap* pmap, const VirtualAddress virt, const std::size_t size_bytes,
                          const PageSize stride, std::uint32_t) noexcept {
  // The request lives on our stack: call_many() does not return before every target is done.
  ShootdownRequest req{
      .pmap = pmap,
      .base_virt = virt,
      .size_bytes = size_bytes,
      .stride = stride,
  };

  (void)hw::ipi::call_many(&is_shootdown_target, &req, &run_shootdown, &req, hw::ipi::Wait::Yes);
}

void remote_shootdown_invlpgb(PageMap* pmap, const VirtualAddress virt,
                              const std::size_t size_bytes, const PageSize stride,
                              const std::uint32_t my_cpu) noexcept {
  if (size_bytes == FLUSH_ALL) [[unlikely]] {
    remote_shootdown_ipi(pmap, virt, size_bytes, stride, my_cpu);
    return;
  }

  // Kernel translations are global: one broadcast without a PCID filter reaches every CPU.
  if (pmap->is_kernel() || virt.is_upper_half()) {
    ShootdownCoordinator::queue_invlpgb(virt, size_bytes, 0, stride);
    hw::tlbsync();
    return;
  }

  // PCIDs are per CPU, so the same map can have a different one everywhere. Broadcast once per
  // distinct PCID; other CPUs using that number for something else just lose a few entries.
  bool sent_broadcast = false;
  std::uint64_t broadcasted_pcids[64] = {0};

  pmap->cpu_tracker().for_each_active_cpu(
      [&](const std::uint32_t cpu, const std::uint16_t remote_pcid) noexcept {
        if (cpu == my_cpu) {
          return;
        }

        const std::size_t word = remote_pcid / 64;
        const std::uint64_t bit = 1ul << (remote_pcid % 64);

        if ((broadcasted_pcids[word] & bit) == 0) {
          ShootdownCoordinator::queue_invlpgb(virt, size_bytes, remote_pcid, stride);
          broadcasted_pcids[word] |= bit;
          sent_broadcast = true;
        }
      });

  if (sent_broadcast) {
    hw::tlbsync();
  }
}
}  // namespace

namespace detail {
DEFINE_STATIC_CALL(tlb_flush_everything, flush_everything_pge);
REGISTER_STATIC_CALL_ALT(tlb_flush_everything, flush_everything_invpcid, hw::Feature::INVPCID);

DEFINE_STATIC_CALL(tlb_flush_pcid, flush_pcid_unsupported);
REGISTER_STATIC_CALL_ALT(tlb_flush_pcid, flush_pcid_invpcid, hw::Feature::INVPCID,
                         hw::Feature::PCID);

DEFINE_STATIC_CALL(tlb_flush_pcid_page, flush_pcid_page_unsupported);
REGISTER_STATIC_CALL_ALT(tlb_flush_pcid_page, flush_pcid_page_invpcid, hw::Feature::INVPCID,
                         hw::Feature::PCID);

DEFINE_STATIC_CALL(tlb_remote_shootdown, remote_shootdown_ipi);
REGISTER_STATIC_CALL_ALT(tlb_remote_shootdown, remote_shootdown_invlpgb, hw::Feature::INVLPGB,
                         !hw::Feature::HYPERVISOR);
}  // namespace detail

void PcidManager::rollover_epoch() noexcept {
  // Bumping the epoch turns every slot stale at once, and the flush guarantees no PCID number
  // carries translations into the new epoch, so they can all be handed out again.
  m_curr_epoch++;
  m_next_pcid = 1;
  detail::tlb_flush_everything();
}

void PcidManager::load(PageMap* pagemap) noexcept {
  const std::uint32_t my_cpu = hw::percpu::id();

  if (!g_paging_features.pcid) {
    pagemap->cpu_tracker().mark(my_cpu, 0);
    hw::write::cr3(hw::CR3::build_legacy(pagemap->root_phys()));
    return;
  }

  const std::uint64_t key = pagemap->generation();
  std::size_t idx = hash(key);

  while (true) {
    Slot& slot = m_table[idx];

    if (slot.epoch == m_curr_epoch && slot.key == key) [[likely]] {
      // Everything that changed since this PCID was last loaded has been invalidated, either by
      // local_invalidate() or by a remote shootdown, so its TLB entries can be kept.
      hw::write::cr3(hw::CR3::build_pcid(pagemap->root_phys(), slot.pcid, true));
      return;
    }

    if (slot.epoch != m_curr_epoch) {
      if (m_next_pcid > MAX_PCID) [[unlikely]] {
        rollover_epoch();
        idx = hash(key);
        continue;
      }

      slot = {.key = key, .epoch = m_curr_epoch, .pcid = m_next_pcid++};
      pagemap->cpu_tracker().mark(my_cpu, slot.pcid);
      hw::write::cr3(hw::CR3::build_pcid(pagemap->root_phys(), slot.pcid, false));
      return;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

std::uint16_t PcidManager::get_active_pcid(const PageMap* pagemap) const noexcept {
  const std::uint64_t key = pagemap->generation();
  std::size_t idx = hash(key);

  while (true) {
    const Slot& slot = m_table[idx];
    if (slot.epoch != m_curr_epoch) {
      return 0;
    }

    if (slot.key == key) {
      return slot.pcid;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

void PcidManager::invalidate_active_pcid(const PageMap* pagemap) noexcept {
  const std::uint64_t key = pagemap->generation();
  std::size_t idx = hash(key);

  while (true) {
    Slot& slot = m_table[idx];
    if (slot.epoch != m_curr_epoch) {
      return;
    }

    if (slot.key == key) {
      // Keep the slot live so probe chains running through it stay intact.
      slot.key = DEAD_KEY;
      return;
    }

    idx = (idx + 1) & TABLE_MASK;
  }
}

void ShootdownCoordinator::local_invalidate(const PageMap* pmap, const VirtualAddress virt,
                                            const std::size_t size,
                                            const PageSize stride) noexcept {
  PcidManager& pcids = hw::percpu::pcid_manager();

  // The kernel half is present in every map and its leaves are global, so INVLPG reaches it
  // whatever this CPU has loaded. For user maps it matters whether the map is the live one.
  const bool kernel = pmap->is_kernel() || virt.is_upper_half();
  const bool current = kernel || hw::read::cr3().extract_address() == pmap->root_phys();
  const std::uint16_t pcid = current ? 0 : pcids.get_active_pcid(pmap);

  if (!current && pcid == 0) {
    return;  // this CPU holds no translations for the map
  }

  const std::size_t step = stride_bytes(stride);
  const std::size_t pages =
      (size == FLUSH_ALL) ? FLUSH_ALL : utils::maths::div_round_up(size, step);

  if (pages > SINGLE_PAGE_FLUSH_CEILING) {
    if (kernel) {
      detail::tlb_flush_everything();
    } else if (current) {
      // Reloading CR3 without NOFLUSH drops the current PCID's non-global entries.
      hw::write::cr3(hw::read::cr3());
    } else if (!detail::tlb_flush_pcid(pcid)) {
      pcids.invalidate_active_pcid(pmap);
    }

    return;
  }

  for (std::size_t i = 0; i < pages; ++i) {
    const VirtualAddress addr = virt + static_cast<VirtualAddress::OffsetType>(i * step);

    if (current) {
      hw::invlpg(addr.value());
    } else if (!detail::tlb_flush_pcid_page(pcid, addr)) {
      pcids.invalidate_active_pcid(pmap);
      return;
    }
  }
}

void ShootdownCoordinator::queue_invlpgb(const VirtualAddress virt, const std::size_t size_bytes,
                                         const std::uint16_t target_pcid,
                                         const PageSize stride) noexcept {
  const bool global = virt.is_upper_half();

  auto rax = hw::InvlpgbAddress{}.with_va_valid();
  if (global) {
    rax.set_include_global(true);
  } else {
    rax.set_pcid_valid(true);
  }

  hw::InvlpgbContext edx{0};
  edx.set_pcid(global ? 0 : target_pcid);

  // INVLPGB only knows 4 KiB and 2 MiB strides. 1 GiB pages get a 2 MiB stride, which simply
  // invalidates each of them several times.
  const bool large = stride != PageSize::Size4K;
  hw::InvlpgbCount ecx{0};
  ecx.set_large_page_stride(large);

  const std::size_t step = large ? PAGE_SIZE_2MB : PAGE_SIZE;
  const std::size_t max_chunk =
      static_cast<std::size_t>(g_paging_features.invlpgb_max_count) + 1;  // count is "extra"

  std::size_t pages = utils::maths::div_round_up(size_bytes, step);
  std::uintptr_t curr_virt = virt.value();

  while (pages > 0) {
    const std::size_t chunk = std::min(pages, max_chunk);

    rax.set_virt_addr(curr_virt >> PAGE_SHIFT_4KB);
    ecx.set_extra_count(static_cast<std::uint16_t>(chunk - 1));
    hw::invlpgb(rax, ecx, edx);

    pages -= chunk;
    curr_virt += chunk * step;
  }
}

void ShootdownCoordinator::broadcast(PageMap* pmap, const VirtualAddress virt,
                                     const std::size_t size_bytes, const PageSize stride) noexcept {
  if (size_bytes == 0) [[unlikely]] {
    return;
  }

  // The local flush and the "everyone but me" decision must be made on the same CPU.
  hw::percpu::preempt_disable();
  const std::uint32_t my_cpu = hw::percpu::id();

  local_invalidate(pmap, virt, size_bytes, stride);
  detail::tlb_remote_shootdown(pmap, virt, size_bytes, stride, my_cpu);

  hw::percpu::preempt_enable();
}

void ShootdownCoordinator::cpu_online() noexcept {
  // Nobody could shoot at us until now, so anything already in the TLB may be stale.
  detail::tlb_flush_everything();
}
}  // namespace kernel::memory::vmm::tlb
