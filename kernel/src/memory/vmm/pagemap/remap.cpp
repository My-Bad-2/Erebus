#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap.hpp"
#include "memory/vmm/tlb.hpp"

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

    if ((expected.raw & (1ull << 7)) != 0) [[unlikely]] {
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
constexpr std::size_t MAX_LOG_PTES = 509; // 4096 - 8 (prev) - 8 (virt) - 4 (count) = 4076 bytes => 509 entries

struct UndoLogNode {
  UndoLogNode *prev;
  VirtualAddress base_virt;
  std::uint32_t count;
  PteSchema original_ptes[509];
};
} // namespace

std::expected<void, Error> PageMap::remap_range(const VirtualAddress start_virt, const PhysicalAddress start_phys,
                                                const std::size_t size_bytes, const AccessFlags flags,
                                                const CacheMode cache, const std::uint8_t pkey) noexcept {
  if (!start_virt.is_aligned(PAGE_SIZE) || !start_phys.is_aligned(PAGE_SIZE) ||
      !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) [[unlikely]] {
    return std::unexpected(Error::InvalidAlignment);
  }

  struct RollbackGuard {
    PageMap &pmap;
    UndoLogNode *tail_log{nullptr};
    bool success{false};
    VirtualAddress start_virt;
    std::size_t attempted_bytes{0};

    ~RollbackGuard() {
      if (!success && tail_log) {
        UndoLogNode *node = tail_log;
        while (node) {
          if (auto pt_res = pmap.walk(node->base_virt, false, PageSize::Size4K)) {
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
    const std::size_t bytes_to_pml1_boundary = PAGE_SIZE_2MB - curr_virt.page_offset(PAGE_SIZE_2MB);
    constexpr std::size_t max_log_bytes = MAX_LOG_PTES * PAGE_SIZE;

    const std::size_t batch_bytes = std::min({remaining, bytes_to_pml1_boundary, max_log_bytes});
    const std::size_t pages_in_batch = batch_bytes / PAGE_SIZE;

    auto log_page = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
    if (!log_page) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    UndoLogNode *current_log = DirectMap::phys_to_virt(*log_page).as<UndoLogNode>();
    current_log->prev = guard.tail_log;
    current_log->base_virt = curr_virt;
    current_log->count = 0;

    guard.tail_log = current_log;

    auto pt_res = walk(curr_virt, false, PageSize::Size4K);
    if (!pt_res) [[unlikely]] {
      return std::unexpected(pt_res.error());
    }

    PageTableEntry *pte = *pt_res;
    for (std::size_t i = 0; i < pages_in_batch; ++i) {
      PteSchema expected = pte[i].load(std::memory_order_acquire);

      while (true) {
        if (expected.small().get<"present">() == 0) [[unlikely]] {
          return std::unexpected(Error::NotMapped);
        }

        current_log->original_ptes[current_log->count] = expected;

        const PhysicalAddress offset_phys = curr_phys + (i * PAGE_SIZE);
        const PteSchema desired = PageTableEntry::build_schema(offset_phys, flags, cache, PageSize::Size4K, pkey);
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
} // namespace kernel::memory::vmm