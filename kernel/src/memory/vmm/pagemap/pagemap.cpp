#include "memory/vmm/pagemap.hpp"

#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "string.h"

#include "memory/memory.hpp"
#include "memory/vmm/tlb.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::vmm {
namespace {
heap::KmemCache *pmap_cache = nullptr;
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

using PatMsrSchema =
    klib::BitfieldSchema<klib::Field<"pat0", 0, 8>, klib::Field<"pat1", 8, 8>, klib::Field<"pat2", 16, 8>,
                         klib::Field<"pat3", 24, 8>, klib::Field<"pat4", 32, 8>, klib::Field<"pat5", 40, 8>,
                         klib::Field<"pat6", 48, 8>, klib::Field<"pat7", 56, 8>>;

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

AccessFlags PageMap::extract_flags(const PteSchema &schema) noexcept {
  auto flags = AccessFlags::None;
  const auto s = schema.small();

  if (s.get<"rw">()) {
    flags = flags | AccessFlags::Write;
  }

  if (s.get<"user">()) {
    flags = flags | AccessFlags::User;
  }

  if (s.get<"global">()) {
    flags = flags | AccessFlags::Global;
  }

  if (!s.get<"nx">()) {
    flags = flags | AccessFlags::Execute;
  }

  if (s.get<"cow">()) {
    flags = flags | AccessFlags::CopyOnWrite;
  }

  if (s.get<"shared">()) {
    flags = flags | AccessFlags::Shared;
  }

  return flags | AccessFlags::Read;
}

CacheMode PageMap::extract_cache(const PteSchema &schema, const bool is_huge) noexcept {
  const auto s = schema.small();
  const bool pwt = s.get<"pwt">() == 1;
  const bool pcd = s.get<"pcd">() == 1;

  const bool pat = is_huge ? (schema.huge().get<"pat">() == 1) : (s.get<"pat">() == 1);

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

std::expected<PageMap *, Error> PageMap::create() noexcept {
  auto pmap_res = pmap_cache->alloc();
  if (!pmap_res) {
    return std::unexpected(Error::OutOfMemory);
  }

  auto *pmap = static_cast<PageMap *>(*pmap_res);

  const auto root_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, 0);
  if (!root_res) {
    pmap_cache->free(pmap);
    return std::unexpected(Error::OutOfMemory);
  }

  const PhysicalAddress root_phys{*root_res};
  pmm::phys_to_page(root_phys)->set_mobility(pmm::PageMobility::Unmovable);

  new (pmap) PageMap{root_phys, max_lvls};

  const PageMap *kmap = get_kernel_pagemap();
  if (kmap) {
    auto *new_root_table = DirectMap::phys_to_virt(root_phys).as<PageTableEntry>();
    auto *kernel_root_table = DirectMap::phys_to_virt(kmap->m_root_phys).as<PageTableEntry>();

    for (std::size_t i = MAX_PAGE_ENTRIES / 2; i < MAX_PAGE_ENTRIES; ++i) {
      PteSchema k_entry = kernel_root_table[i].load(std::memory_order_relaxed);
      new_root_table[i].store(k_entry, std::memory_order_relaxed);
    }
  }
  return pmap;
}

void PageMap::destroy(PageMap *map) noexcept {
  if (!map) {
    return;
  }

  free_table_recursive(map->m_root_phys, map->m_lvls);
  map->~PageMap();
  pmap_cache->free(map);
}

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

  const hw::CR4Schema cr4 = hw::read::cr4();
  const bool la57_active = cr4.get<"la57">();
  max_lvls = la57_active ? 5 : 4;
}

void initialize_hw() noexcept {
  if (!pmap_cache) {
    const auto res = heap::KmemCache::create("pagemap", sizeof(PageMap), alignof(PageMap));
    if (!res) {
      utils::logger::fatal("Unable to create cache for pagemap!\n");
    }

    pmap_cache = *res;
  }

  hw::EferSchema efer = hw::read::efer();
  efer.set_mut<"nxe">(nxe_supported ? 1 : 0);
  efer.set_mut<"tce">(tce_supported ? 1 : 0);
  hw::write::efer(efer);

  hw::CR0Schema cr0 = hw::read::cr0();
  cr0.set_mut<"em">(0); // emulation off
  cr0.set_mut<"mp">(1); // monitor co-processor on
  cr0.set_mut<"ne">(1); // native exceptions on
  cr0.set_mut<"wp">(1); // kernel write protection on
  cr0.set_mut<"pg">(1); // paging on
  hw::write::cr0(cr0);

  hw::CR4Schema cr4 = hw::read::cr4();
  cr4.set_mut<"pae">(1);
  cr4.set_mut<"pge">(pge_supported ? 1 : 0);
  cr4.set_mut<"smep">(smep_supported ? 1 : 0);
  cr4.set_mut<"smap">(smap_supported ? 1 : 0);
  cr4.set_mut<"umip">(umip_supported ? 1 : 0);
  cr4.set_mut<"pke">(pke_supported ? 1 : 0);
  cr4.set_mut<"pcide">(pcid_supported ? 1 : 0);
  hw::write::cr4(cr4);

  constexpr std::uint32_t MSR_PAT = 0x00000277;

  PatMsrSchema pat{0};
  pat.set_mut<"pat0">(std::to_underlying(PatMemoryType::WriteBack));
  pat.set_mut<"pat1">(std::to_underlying(PatMemoryType::WriteThrough));
  pat.set_mut<"pat2">(std::to_underlying(PatMemoryType::UncacheableMinus));
  pat.set_mut<"pat3">(std::to_underlying(PatMemoryType::Uncacheable));
  pat.set_mut<"pat4">(std::to_underlying(PatMemoryType::WriteCombining));
  pat.set_mut<"pat5">(std::to_underlying(PatMemoryType::WriteProtected));
  pat.set_mut<"pat6">(std::to_underlying(PatMemoryType::UncacheableMinus));
  pat.set_mut<"pat7">(std::to_underlying(PatMemoryType::Uncacheable));

  hw::write::msr(MSR_PAT, static_cast<std::uint64_t>(pat));
}
} // namespace kernel::memory::vmm