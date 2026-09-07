#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/pagemap.hpp"
#include "memory/vmm/pagemap/tlb.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> PageMap::remap_1gb_as_2mb(const VirtualAddress virt, const PhysicalAddress new_phys,
                                                     const AccessFlags flags, const CacheMode cache,
                                                     const std::uint8_t pkey) noexcept {
  if (!virt.is_aligned(PAGE_SIZE_1GB) || !new_phys.is_aligned(PAGE_SIZE_1GB)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto pdpte_res = walk(virt, false, PageSize::Size1G);
  if (!pdpte_res) [[unlikely]] {
    return std::unexpected(pdpte_res.error());
  }

  PageTableEntry *pdpte = *pdpte_res;

  const auto new_page_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
  if (!new_page_res) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  const PhysicalAddress new_pml2_phys{*new_page_res};
  pmm::phys_to_page(new_pml2_phys)->set_mobility(pmm::PageMobility::Unmovable);
  auto *pml2_table = DirectMap::phys_to_virt(new_pml2_phys).as<PageTableEntry>();

  for (std::uint32_t i = 0; i < MAX_PAGE_ENTRIES; ++i) {
    const PhysicalAddress offset_phys = new_phys + (i * PAGE_SIZE_2MB);
    const PteSchema child_schema = PageTableEntry::build_schema(offset_phys, flags, cache, PageSize::Size2M, pkey);

    pml2_table[i].store(child_schema, std::memory_order_relaxed);
  }

  PteLeafSchema dir_leaf{};
  dir_leaf.set_mut<"present">(1);
  dir_leaf.set_mut<"pfn">(new_pml2_phys.value() >> 12);
  dir_leaf.set_mut<"rw">(1);
  dir_leaf.set_mut<"user">(1);

  const PteSchema desired{static_cast<std::uint64_t>(dir_leaf)};
  PteSchema expected = pdpte->load(std::memory_order_acquire);

  while (true) {
    if (expected.small().get<"present">() == 0) [[unlikely]] {
      pmm::free_pages(new_pml2_phys, 0);
      return std::unexpected(Error::NotMapped);
    }

    if ((expected.raw & (1ul << 7)) != 0) [[unlikely]] {
      pmm::free_pages(new_pml2_phys, 0);
      return std::unexpected(Error::InvalidFlags);
    }

    if (pdpte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
      tlb::ShootdownCoordinator::broadcast(this, virt, PAGE_SIZE_1GB, true);

      const PhysicalAddress old_pml2_phys{expected.small().get<"pfn">() << 12};
      // TODO: RCU deferred free.
      pmm::free_pages(old_pml2_phys, 0);

      return {};
    }
  }
}

std::expected<void, Error> PageMap::remap(const VirtualAddress virt, const PhysicalAddress new_phys,
                                          const AccessFlags flags, const CacheMode cache, const std::uint8_t pkey,
                                          const PageSize size) noexcept {
  const bool is_kernel_addr = (virt.value() >= DirectMap::s_hhdm_base);
  if (is_kernel_addr && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  const std::size_t page_size_bytes = size_to_bytes(size);

  if (!virt.is_aligned(page_size_bytes) || !new_phys.is_aligned(page_size_bytes)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size == PageSize::Size1G && !supports_1gb_pages) [[unlikely]] {
    return remap_1gb_as_2mb(virt, new_phys, flags, cache, pkey);
  }

  const auto pte_res = walk(virt, false, size);
  if (!pte_res) [[unlikely]] {
    return std::unexpected(pte_res.error());
  }

  PageTableEntry *pte = *pte_res;
  PteSchema expected = pte->load(std::memory_order_acquire);

  while (true) {
    if (expected.small().get<"present">() == 0) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    const PteSchema desired = PageTableEntry::build_schema(new_phys, flags, cache, size, pkey);
    if (pte->cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
      const bool is_huge = (size != PageSize::Size4K);
      tlb::ShootdownCoordinator::broadcast(this, virt, page_size_bytes, is_huge);
      return {};
    }
  }
}

namespace {
constexpr std::size_t MAX_LOG_PTES = 508;

struct UndoLogNode {
  UndoLogNode *prev;
  VirtualAddress base_virt;
  std::uint32_t count;
  PageSize page_size;
  PteSchema original_ptes[MAX_LOG_PTES];
};
} // namespace

struct RollbackGuard {
  PageMap &pmap;
  UndoLogNode *tail_log{nullptr};
  bool success{false};
  VirtualAddress start_virt;
  std::size_t attempted_bytes{0};

  ~RollbackGuard() {
    if (!success && tail_log) {
      const UndoLogNode *node = tail_log;
      while (node) {
        if (auto pt_res = pmap.walk(node->base_virt, false, node->page_size)) {
          PageTableEntry *pte = *pt_res;
          for (std::uint32_t i = 0; i < node->count; ++i) {
            pte[i].store(node->original_ptes[i], std::memory_order_release);
          }
        }

        node = node->prev;
      }

      if (attempted_bytes > 0) {
        tlb::ShootdownCoordinator::broadcast(&pmap, start_virt, attempted_bytes, attempted_bytes >= PAGE_SIZE_2MB);
      }
    }

    while (tail_log) {
      UndoLogNode *prev = tail_log->prev;
      if (auto phys = DirectMap::virt_to_phys(VirtualAddress{tail_log})) {
        pmm::free_pages(*phys, 0);
      }

      tail_log = prev;
    }
  }
};

std::expected<void, Error> PageMap::remap_range(const VirtualAddress start_virt, const PhysicalAddress start_phys,
                                                const std::size_t size_bytes, const AccessFlags flags,
                                                const CacheMode cache, const std::uint8_t pkey) noexcept {
  const bool is_kernel_addr = (start_virt.value() >= DirectMap::s_hhdm_base);
  if (is_kernel_addr && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  if (!start_virt.is_aligned(PAGE_SIZE) || !start_phys.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  RollbackGuard guard{
      .pmap = *this,
      .tail_log = nullptr,
      .success = false,
      .start_virt = start_virt,
      .attempted_bytes = 0,
  };

  VirtualAddress curr_virt = start_virt;
  PhysicalAddress curr_phys = start_phys;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    auto trans = translate(curr_virt);
    if (!trans) [[unlikely]] {
      return std::unexpected(Error::NotMapped);
    }

    PageSize target_size = PageSize::Size4K;
    std::size_t step_size = PAGE_SIZE;
    std::size_t region_coverage = PAGE_SIZE_2MB;

    if (trans->mapped_size == PageSize::Size1G && remaining >= PAGE_SIZE_1GB && curr_virt.is_aligned(PAGE_SIZE_1GB) &&
        curr_phys.is_aligned(PAGE_SIZE_1GB)) {
      target_size = PageSize::Size1G;
      step_size = PAGE_SIZE_1GB;
      region_coverage = 512ul * PAGE_SIZE_1GB;
    } else if (trans->mapped_size >= PageSize::Size2M && remaining >= PAGE_SIZE_2MB &&
               curr_virt.is_aligned(PAGE_SIZE_2MB) && curr_phys.is_aligned(PAGE_SIZE_2MB)) {
      target_size = PageSize::Size2M;
      step_size = PAGE_SIZE_2MB;
      region_coverage = PAGE_SIZE_1GB;
    }

    const std::size_t bytes_to_boundary = region_coverage - curr_virt.page_offset(region_coverage);
    const std::size_t max_log_bytes = MAX_LOG_PTES * step_size;

    const std::size_t batch_bytes = std::min({remaining, bytes_to_boundary, max_log_bytes});
    const std::size_t pages_in_batch = batch_bytes / step_size;

    auto pt_res = walk(curr_virt, false, target_size);
    if (!pt_res) [[unlikely]] {
      return std::unexpected(pt_res.error());
    }

    auto log_page = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
    if (!log_page) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    UndoLogNode *current_log = DirectMap::phys_to_virt(*log_page).as<UndoLogNode>();
    current_log->prev = guard.tail_log;
    current_log->base_virt = curr_virt;
    current_log->count = 0;
    current_log->page_size = target_size;

    guard.tail_log = current_log;

    PageTableEntry *pte = *pt_res;
    for (std::size_t i = 0; i < pages_in_batch; ++i) {
      PteSchema expected = pte[i].load(std::memory_order_acquire);

      while (true) {
        if (expected.small().get<"present">() == 0) [[unlikely]] {
          return std::unexpected(Error::NotMapped);
        }

        current_log->original_ptes[current_log->count] = expected;

        const PhysicalAddress offset_phys = curr_phys + (i * step_size);
        const PteSchema desired = PageTableEntry::build_schema(offset_phys, flags, cache, target_size, pkey);
        if (pte[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
          current_log->count++;
          break;
        }
      }
    }

    curr_virt += batch_bytes;
    curr_phys += batch_bytes;
    remaining -= batch_bytes;
    guard.attempted_bytes += batch_bytes;
  }

  guard.success = true;
  tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes, size_bytes >= PAGE_SIZE_2MB);

  return {};
}

std::expected<void, Error> PageMap::protect_virtual_range(VirtualAddress start_virt, std::size_t size_bytes,
                                                          const AccessFlags flags) noexcept {
  const bool is_kernel_addr = (start_virt.value() >= DirectMap::s_hhdm_base);
  if (is_kernel_addr && has_flag(flags, AccessFlags::User)) [[unlikely]] {
    return std::unexpected(Error::SecurityViolation);
  }

  if (!start_virt.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  RollbackGuard guard{
      .pmap = *this,
      .tail_log = nullptr,
      .success = false,
      .start_virt = start_virt,
      .attempted_bytes = 0,
  };

  VirtualAddress curr_virt = start_virt;
  std::size_t remaining = size_bytes;

  while (remaining > 0) {
    const auto trans = translate(curr_virt);
    if (!trans) {
      curr_virt += PAGE_SIZE;
      remaining -= PAGE_SIZE;
      continue; // Skip unmapped holes
    }

    PageSize target_size = PageSize::Size4K;
    std::size_t step_size = PAGE_SIZE;
    std::size_t region_coverage = PAGE_SIZE_2MB;

    if (trans->mapped_size == PageSize::Size1G && remaining >= PAGE_SIZE_1GB && curr_virt.is_aligned(PAGE_SIZE_1GB)) {
      target_size = PageSize::Size1G;
      step_size = PAGE_SIZE_1GB;
      region_coverage = 512ul * PAGE_SIZE_1GB;
    } else if (trans->mapped_size >= PageSize::Size2M && remaining >= PAGE_SIZE_2MB &&
               curr_virt.is_aligned(PAGE_SIZE_2MB)) {
      target_size = PageSize::Size2M;
      step_size = PAGE_SIZE_2MB;
      region_coverage = PAGE_SIZE_1GB;
    }

    const std::size_t bytes_to_boundary = region_coverage - curr_virt.page_offset(region_coverage);
    const std::size_t max_log_bytes = MAX_LOG_PTES * step_size;

    const std::size_t batch_bytes = std::min({remaining, bytes_to_boundary, max_log_bytes});
    const std::size_t pages_in_batch = batch_bytes / step_size;

    auto pt_res = walk(curr_virt, false, target_size);
    if (!pt_res) {
      curr_virt += batch_bytes;
      remaining -= batch_bytes;
      continue;
    }

    PageTableEntry *pte = *pt_res;

    auto log_page = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
    if (!log_page) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    UndoLogNode *current_log = DirectMap::phys_to_virt(*log_page).as<UndoLogNode>();
    current_log->prev = guard.tail_log;
    current_log->base_virt = curr_virt;
    current_log->count = 0;
    current_log->page_size = target_size;

    guard.tail_log = current_log;

    for (std::size_t i = 0; i < pages_in_batch; ++i) {
      PteSchema expected = pte[i].load(std::memory_order_acquire);

      while (true) {
        if (expected.small().get<"present">() == 0) {
          break;
        }

        current_log->original_ptes[current_log->count] = expected;

        PteLeafSchema modified_leaf{expected.raw};
        modified_leaf.set_mut<"rw">(has_flag(flags, AccessFlags::Write) ? 1 : 0);
        modified_leaf.set_mut<"nx">(has_flag(flags, AccessFlags::Execute) ? 0 : 1);
        modified_leaf.set_mut<"user">(has_flag(flags, AccessFlags::User) ? 1 : 0);

        const PteSchema desired{static_cast<std::uint64_t>(modified_leaf)};

        if (pte[i].cas(expected, desired, std::memory_order_release, std::memory_order_acquire)) [[likely]] {
          current_log->count++;
          break;
        }
      }
    }

    curr_virt += batch_bytes;
    remaining -= batch_bytes;
    guard.attempted_bytes += batch_bytes;
  }

  guard.success = true;
  tlb::ShootdownCoordinator::broadcast(this, start_virt, size_bytes, false);

  return {};
}
} // namespace kernel::memory::vmm