#include "memory/vmm/pagemap/pagemap.hpp"

#include <cpuid.h>

#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm {
namespace {
enum class PatMemoryType : std::uint64_t {
  Uncacheable = 0x00,
  WriteCombining = 0x01,
  WriteThrough = 0x04,
  WriteProtected = 0x05,
  WriteBack = 0x06,
  UncacheableMinus = 0x07
};

class PatMsr {
  std::uint64_t m_data;

 public:
  constexpr explicit PatMsr(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, pat0, 0, 8)
  BF_RW(std::uint8_t, pat1, 8, 8)
  BF_RW(std::uint8_t, pat2, 16, 8)
  BF_RW(std::uint8_t, pat3, 24, 8)
  BF_RW(std::uint8_t, pat4, 32, 8)
  BF_RW(std::uint8_t, pat5, 40, 8)
  BF_RW(std::uint8_t, pat6, 48, 8)
  BF_RW(std::uint8_t, pat7, 56, 8)
};

// What each PAT index means with the layout programmed in initialize_hw().
constexpr CacheMode PAT_INDEX_TO_MODE[8] = {
    CacheMode::WriteBack,        CacheMode::WriteThrough,   CacheMode::UncacheableMinus,
    CacheMode::Uncacheable,      CacheMode::WriteCombining, CacheMode::WriteProtected,
    CacheMode::UncacheableMinus, CacheMode::Uncacheable,
};

// Frees a table and every table below it. Leaf frames belong to whoever mapped them.
void free_table_recursive(const PhysicalAddress table_phys, const std::uint8_t lvl,
                          const std::uint32_t first = 0, const std::uint32_t last = 512) noexcept {
  if (lvl > 1) {
    const auto* table = DirectMap::phys_to_virt(table_phys).as<PageTableEntry>();
    for (std::uint32_t i = first; i < last; ++i) {
      const PTEntry entry = table[i].load(std::memory_order_relaxed);
      if (entry.get_present() && !entry.get_huge()) {
        free_table_recursive(PhysicalAddress{entry.get_pfn_4k() << PAGE_SHIFT_4KB},
                             static_cast<std::uint8_t>(lvl - 1));
      }
    }
  }

  pmm::free_pages(table_phys, 0);
}
}  // namespace

AccessFlags PageMap::extract_flags(const PTEntry& s) noexcept {
  auto flags = AccessFlags::Read;

  if (s.get_rw()) {
    flags |= AccessFlags::Write;
  }

  if (s.get_user()) {
    flags |= AccessFlags::User;
  }

  if (s.get_global()) {
    flags |= AccessFlags::Global;
  }

  if (!s.get_nx()) {
    flags |= AccessFlags::Execute;
  }

  if (s.get_cow()) {
    flags |= AccessFlags::CopyOnWrite;
  }

  if (s.get_shared()) {
    flags |= AccessFlags::Shared;
  }

  if (s.get_stack()) {
    flags |= AccessFlags::Stack;
  }

  return flags;
}

CacheMode PageMap::extract_cache(const PTEntry& s, const bool is_huge) noexcept {
  const bool pat = is_huge ? s.get_pat_large() : s.get_pat_4k();
  const std::uint32_t index = (pat ? 4u : 0u) | (s.get_pcd() ? 2u : 0u) | (s.get_pwt() ? 1u : 0u);
  return PAT_INDEX_TO_MODE[index];
}

AccessFlags PageMap::leaf_flags(const VirtualAddress virt, const AccessFlags flags) noexcept {
  // Kernel mappings are identical in every address space, so they can survive CR3 switches.
  // User mappings must not: a global user page would leak into the next process.
  return virt.is_upper_half() ? (flags | AccessFlags::Global) : (flags & ~AccessFlags::Global);
}

PageMap::PageMap() noexcept
    : m_generation{s_next_generation.fetch_add(1, std::memory_order_relaxed)},
      m_lvls{g_paging_features.levels} {
  const auto root_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
  if (!root_res) {
    utils::logger::fatal("Unable to allocate page for top pml table!\n");
  }

  m_root_phys = *root_res;
  auto* root = DirectMap::phys_to_virt(m_root_phys).as<PageTableEntry>();

  if (s_kernel_root.value() != 0) [[likely]] {
    // Share the kernel's tables. They are all present (see below), so the copy can never miss
    // a kernel mapping created later.
    const auto* kernel_root = DirectMap::phys_to_virt(s_kernel_root).as<PageTableEntry>();
    for (std::uint32_t i = KERNEL_HALF_START; i < MAX_PAGE_ENTRIES; ++i) {
      root[i].store(kernel_root[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    return;
  }

  // This is the kernel map. Populate the whole upper half now (1 MiB of tables) so that no new
  // top-level entry ever has to be added after other address spaces copied the half.
  for (std::uint32_t i = KERNEL_HALF_START; i < MAX_PAGE_ENTRIES; ++i) {
    const auto table = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
    if (!table) {
      utils::logger::fatal("Unable to allocate the kernel's top-level page tables!\n");
    }

    root[i].store(PageTableEntry::build_directory(*table, false), std::memory_order_relaxed);
  }

  s_kernel_root = m_root_phys;
}

PageMap::~PageMap() noexcept {
  if (is_kernel()) [[unlikely]] {
    utils::logger::fatal("Attempted to destroy the kernel page map!\n");
  }

  // The upper half belongs to the kernel and is shared by everyone; only free our own half.
  free_table_recursive(m_root_phys, m_lvls, 0, KERNEL_HALF_START);
}

void early_initialize_hw() noexcept {
  const hw::CpuInfo* info = hw::profile_manager.get_current();
  PagingFeatures& f = g_paging_features;

  f.huge_1g = info->has<hw::Feature::GIB>();
  f.smap = info->has<hw::Feature::SMAP>();
  f.smep = info->has<hw::Feature::SMEP>();
  f.nx = info->has<hw::Feature::NX>();
  f.pge = info->has<hw::Feature::PGE>();
  f.umip = info->has<hw::Feature::UMIP>();
  f.pke = info->has<hw::Feature::PKU>();
  f.pcid = info->has<hw::Feature::PCID>();
  f.invpcid = info->has<hw::Feature::INVPCID>();
  f.invlpgb = info->has<hw::Feature::INVLPGB>();
  f.tce = info->has<hw::Feature::TCE>();

  if (f.invlpgb) {
    std::uint32_t eax, ebx, ecx, edx;
    __cpuid(0x80000008, eax, ebx, ecx, edx);
    f.invlpgb_max_count = static_cast<std::uint16_t>(edx & 0xFFFF);
  }

  f.levels = hw::read::cr4().get_la57() ? 5 : 4;
}

void initialize_hw() noexcept {
  const PagingFeatures& f = g_paging_features;

  hw::EFER efer = hw::read::efer();
  efer.set_nxe(f.nx);
  efer.set_tce(f.tce);
  hw::write::efer(efer);

  hw::CR0 cr0 = hw::read::cr0()
                    .without_em()    // emulation off
                    .with_mp(true)   // monitor co-processor on
                    .with_ne(true)   // native exceptions on
                    .with_wp(true)   // kernel write protection on
                    .with_pg(true);  // paging on
  hw::write::cr0(cr0);

  // Turning PCIDE on requires CR3[11:0] == 0, which holds for the tables Limine hands us.
  hw::CR4 cr4 = hw::read::cr4();
  cr4.set_pae(true);
  cr4.set_pge(f.pge);
  cr4.set_smep(f.smep);
  cr4.set_smap(f.smap);
  cr4.set_umip(f.umip);
  cr4.set_pke(f.pke);
  cr4.set_pcide(f.pcid);
  hw::write::cr4(cr4);

  // Indices 0-3 keep their power-on meaning so PWT/PCD alone still behave as documented;
  // build_schema() and PAT_INDEX_TO_MODE depend on exactly this layout.
  constexpr std::uint32_t MSR_PAT = 0x00000277;

  PatMsr pat{0};
  pat.set_pat0(std::to_underlying(PatMemoryType::WriteBack));
  pat.set_pat1(std::to_underlying(PatMemoryType::WriteThrough));
  pat.set_pat2(std::to_underlying(PatMemoryType::UncacheableMinus));
  pat.set_pat3(std::to_underlying(PatMemoryType::Uncacheable));
  pat.set_pat4(std::to_underlying(PatMemoryType::WriteCombining));
  pat.set_pat5(std::to_underlying(PatMemoryType::WriteProtected));
  pat.set_pat6(std::to_underlying(PatMemoryType::UncacheableMinus));
  pat.set_pat7(std::to_underlying(PatMemoryType::Uncacheable));
  hw::write::msr(MSR_PAT, pat.raw());
}
}  // namespace kernel::memory::vmm
