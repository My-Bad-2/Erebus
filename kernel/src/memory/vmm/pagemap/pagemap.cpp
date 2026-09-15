#include "memory/vmm/pagemap/pagemap.hpp"

#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/tlb.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm {
namespace {
std::uint8_t max_lvls = 0;

bool nxe_supported = false;
bool pge_supported = false;
bool umip_supported = false;
bool smep_supported = false;
bool smap_supported = false;
bool pke_supported = false;
bool pcid_supported = false;
bool tce_supported = false;

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

void free_table_recursive(const PhysicalAddress table_phys, const std::uint8_t lvl) noexcept {
  if (lvl > 1) {
    const auto *table = DirectMap::phys_to_virt(table_phys).as<PageTableEntry>();
    for (std::uint32_t i = 0; i < 512; ++i) {
      if (table[i].is_present() && !table[i].is_huge()) {
        if (auto child_phys = table[i].extract_address(PageSize::Size4K)) {
          free_table_recursive(*child_phys, static_cast<std::uint8_t>(lvl - 1));
        }
      }
    }
  }

  pmm::free_pages(table_phys, 0);
}
} // namespace

AccessFlags PageMap::extract_flags(const PTEntry &s) noexcept {
  auto flags = AccessFlags::None;

  if (s.get_rw()) {
    flags = flags | AccessFlags::Write;
  }

  if (s.get_user()) {
    flags = flags | AccessFlags::User;
  }

  if (s.get_global()) {
    flags = flags | AccessFlags::Global;
  }

  if (!s.get_nx()) {
    flags = flags | AccessFlags::Execute;
  }

  if (s.get_cow()) {
    flags = flags | AccessFlags::CopyOnWrite;
  }

  if (s.get_shared()) {
    flags = flags | AccessFlags::Shared;
  }

  if (s.get_stack()) {
    flags = flags | AccessFlags::Stack;
  }

  return flags | AccessFlags::Read;
}

CacheMode PageMap::extract_cache(const PTEntry &s, const bool is_huge) noexcept {
  const bool pwt = s.get_pwt() == 1;
  const bool pcd = s.get_pcd() == 1;

  const bool pat = is_huge ? s.get_pat_large() : s.get_pat_4k();

  if (!pat && !pcd && !pwt) {
    return CacheMode::WriteBack;
  }

  if (!pat && !pcd && pwt) {
    return CacheMode::WriteThrough;
  }

  if (!pat && pcd && !pwt) {
    return CacheMode::UncacheableMinus;
  }

  if (!pat && pcd && pwt) {
    return CacheMode::Uncacheable;
  }

  if (pat && !pcd && !pwt) {
    return CacheMode::WriteCombining;
  }

  return CacheMode::WriteBack;
}

PageMap::PageMap() noexcept : m_lvls{max_lvls} {
  const auto root_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Unmovable, 0);
  if (!root_res) {
    utils::logger::fatal("Unable to allocate page for top pml table!\n");
  }

  const PhysicalAddress root_phys{*root_res};
  m_root_phys = root_phys;

  if (s_kernel_root) [[likely]] {
    auto *new_root_table = DirectMap::phys_to_virt(root_phys).as<PageTableEntry>();
    auto *kernel_root_table = s_kernel_root.as<PageTableEntry>();

    for (std::size_t i = MAX_PAGE_ENTRIES / 2; i < MAX_PAGE_ENTRIES; ++i) {
      PTEntry k_entry = kernel_root_table[i].load(std::memory_order_relaxed);
      new_root_table[i].store(k_entry, std::memory_order_relaxed);
    }
  } else {
    s_kernel_root = DirectMap::phys_to_virt(root_phys);
  }
}

PageMap::~PageMap() noexcept { free_table_recursive(m_root_phys, m_lvls); }

void early_initialize_hw() noexcept {
  const hw::CpuInfo *info = hw::profile_manager.get_current();

  tlb::initialize();

  supports_1gb_pages = info->has<hw::Feature::GIB>();
  smap_supported = info->has<hw::Feature::SMAP>();
  smep_supported = info->has<hw::Feature::SMEP>();
  nxe_supported = info->has<hw::Feature::NX>();
  pge_supported = info->has<hw::Feature::PGE>();
  umip_supported = info->has<hw::Feature::UMIP>();
  pke_supported = info->has<hw::Feature::PKU>();
  pcid_supported = info->has<hw::Feature::PCID>();
  tce_supported = info->has<hw::Feature::TCE>();

  const hw::CR4 cr4 = hw::read::cr4();
  const bool la57_active = cr4.get_la57();
  max_lvls = la57_active ? 5 : 4;
}

void initialize_hw() noexcept {
  hw::EFER efer = hw::read::efer();
  efer.set_nxe(nxe_supported);
  efer.set_tce(tce_supported);
  hw::write::efer(efer);

  hw::CR0 cr0 = hw::read::cr0()
                    .without_em()   // emulation off
                    .with_mp(true)  // monitor co-processor on
                    .with_ne(true)  // native exceptions on
                    .with_wp(true)  // kernel write protection on
                    .with_pg(true); // paging on
  hw::write::cr0(cr0);

  hw::CR4 cr4 = hw::read::cr4();
  cr4.set_pae(true);
  cr4.set_pge(pge_supported);
  cr4.set_smep(smep_supported);
  cr4.set_smap(smap_supported);
  cr4.set_umip(umip_supported);
  cr4.set_pke(pke_supported);
  cr4.set_pcide(pcid_supported);
  hw::write::cr4(cr4);

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
} // namespace kernel::memory::vmm