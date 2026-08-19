#include "hal/cpu_info.hpp"

#include "utils/lock.hpp"

namespace kernel::hw {
namespace {
constexpr std::uint32_t decode_amd_l2_assoc(std::uint32_t encoded) noexcept {
  switch (encoded) {
  case 0x0:
    return 0; // Disabled
  case 0x1:
    return 1; // Direct mapped
  case 0x2:
    return 2; // 2-way
  case 0x4:
    return 4; // 4-way
  case 0x6:
    return 8; // 8-way
  case 0x8:
    return 16; // 16-way
  case 0xA:
    return 32; // 32-way
  case 0xB:
    return 48; // 48-way
  case 0xC:
    return 64; // 64-way
  case 0xD:
    return 96; // 96-way
  case 0xE:
    return 128; // 128-way
  case 0xF:
    return 0xFF; // Fully associative
  default:
    return 0;
  }
}
} // namespace

void CpuInfo::parse_brand() noexcept {
  if (m_max_ext_leaf >= 0x80000004) {
    for (std::uint32_t i = 0; i < 3; ++i) {
      auto r = query(0x80000002 + i);
      __builtin_memcpy(m_brand_string.data() + (i * 16), &r, 16);
    }
  }
}

void CpuInfo::parse_caches() noexcept {
  const std::uint32_t cache_leaf = (m_vendor == Vendor::Amd) ? 0x8000001D : 0x04;

  if ((m_vendor == Vendor::Amd && m_max_ext_leaf >= cache_leaf) ||
      (m_vendor == Vendor::Intel && m_max_leaf >= cache_leaf)) {
    for (std::uint32_t i = 0; i < m_caches.size(); ++i) {
      const auto [eax, ebx, ecx, edx] = query(cache_leaf, i);
      const std::uint32_t type = eax & 0x1F;
      if (type == 0) {
        break;
      }

      CacheInfo c{};
      c.type = static_cast<CacheType>(type);
      c.level = (eax >> 5) & 0x7;
      c.ways = ((ebx >> 22) & 0x3FF) + 1;
      c.line_size = (ebx & 0xFFF) + 1;
      const std::uint32_t partitions = ((ebx >> 12) & 0x3FF) + 1;
      c.sets = ecx + 1;
      c.size_bytes = c.ways * partitions * c.line_size * c.sets;

      m_caches[m_num_caches++] = c;
    }
  }
}

void CpuInfo::parse_tlb() noexcept {
  if (m_vendor == Vendor::Intel && m_max_leaf >= 0x18) {
    for (uint32_t i = 0; i < m_tlbs.size(); ++i) {
      const auto r = query(0x18, i);
      if ((r.edx & 0x1F) == 0) {
        break; // Type 0 = Null
      }

      TlbInfo t{};
      t.type = static_cast<CacheType>(r.edx & 0x1F);
      t.ways = (r.ebx >> 16) & 0xFFFF;
      t.entries = r.ebx & 0xFFFF;

      const std::uint32_t page_mask = r.ecx & 0xF;
      if (page_mask & 1) {
        t.page_size = PageSize::Size4K;
      } else if (page_mask & 2) {
        t.page_size = PageSize::Size2M;
      } else if (page_mask & 4) {
        t.page_size = PageSize::Size4M;
      } else if (page_mask & 8) {
        t.page_size = PageSize::Size1G;
      } else {
        t.page_size = PageSize::Unknown;
      }

      m_tlbs[m_num_tlbs++] = t;
    }

    return;
  }

  if (m_vendor == Vendor::Amd) {
    auto push = [this](const std::uint32_t reg, const PageSize size, const bool is_l2) {
      if (m_num_tlbs < m_tlbs.size()) {
        const std::uint32_t d_entries = is_l2 ? ((reg >> 16) & 0xFFF) : ((reg >> 16) & 0xFF);
        if (d_entries > 0) {
          m_tlbs[m_num_tlbs++] = {
              .type = CacheType::Data,
              .page_size = size,
              .entries = d_entries,
              .ways = is_l2 ? decode_amd_l2_assoc((reg >> 28) * 0xF) : ((reg >> 24) & 0xFF),
          };
        }
      }

      if (m_num_tlbs < m_tlbs.size()) {
        const std::uint32_t i_entries = is_l2 ? (reg & 0xfff) : (reg & 0xff);
        if (i_entries > 0) {
          m_tlbs[m_num_tlbs++] = {
              .type = CacheType::Instruction,
              .page_size = size,
              .entries = i_entries,
              .ways = is_l2 ? decode_amd_l2_assoc((reg >> 12) & 0xF) : ((reg >> 8) & 0xFF),
          };
        }
      }
    };

    // Leaf 0x80000005: L1 TLBs
    if (m_max_ext_leaf >= 0x80000005) {
      const auto r = query(0x80000005);
      push(r.ebx, PageSize::Size4K, false);
      push(r.eax, PageSize::Size2M, false);
    }

    // Leaf 0x80000006: L2 TLBs
    if (m_max_ext_leaf >= 0x80000006) {
      const auto r = query(0x80000006);
      push(r.ebx, PageSize::Size4K, true);
      push(r.eax, PageSize::Size2M, true);
    }

    // Leaf 0x80000019: 1GB Page TLBs (L1 and L2)
    if (m_max_ext_leaf >= 0x80000019) {
      const auto r = query(0x80000019);
      push(r.eax, PageSize::Size1G, false);
      push(r.ebx, PageSize::Size1G, true);
    }
  }
}

void CpuInfo::parse_topology() noexcept {
  const std::uint32_t topo_leaf = (m_max_leaf >= 0x1F) ? 0x1F : ((m_max_leaf >= 0xB) ? 0xB : 0);

  if (topo_leaf != 0) {
    for (std::uint32_t i = 0; i < m_topology.size(); ++i) {
      const auto r = query(topo_leaf, i);
      const std::uint32_t type = (r.ecx >> 8) & 0xFF;
      if (type == 0) {
        break;
      }

      TopologyInfo t{};
      t.level_type = static_cast<TopoLevel>(type);
      t.id_shift = r.eax & 0x1F;
      t.logical_processors = r.ebx & 0xFFFF;

      m_topology[m_num_topology++] = t;
    }
  } else if (m_vendor == Vendor::Amd && m_max_ext_leaf >= 0x8000001E) {
    if (m_num_topology < m_topology.size()) {
      const auto r = query(0x8000001E);

      TopologyInfo t{};
      t.level_type = TopoLevel::Core;
      t.logical_processors = ((r.ebx >> 8) & 0xFF) + 1;
      m_topology[m_num_topology++] = t;
    }
  }
}

void CpuInfo::parse_hypervisor() noexcept {
  if (m_is_virtualized) {
    const auto r = query(0x40000000);
    std::array<char, 13> hv{};
    __builtin_memcpy(hv.data() + 0, &r.ebx, 4);
    __builtin_memcpy(hv.data() + 4, &r.ecx, 4);
    __builtin_memcpy(hv.data() + 8, &r.edx, 4);

    std::string_view hv_sv{hv.data()};
    if (hv_sv == "KVMKVMKVM\0\0\0") {
      m_hypervisor = Hypervisor::Kvm;
    } else if (hv_sv == "VMwareVMware") {
      m_hypervisor = Hypervisor::Vmware;
    } else if (hv_sv == "Microsoft Hv") {
      m_hypervisor = Hypervisor::HyperV;
    } else if (hv_sv == "XenVMMXenVMM") {
      m_hypervisor = Hypervisor::Xen;
    } else if (hv_sv == "TCGTCGTCGTCG") {
      m_hypervisor = Hypervisor::Qemu;
    } else {
      m_hypervisor = Hypervisor::Unknown;
    }
  }
}

void CpuInfo::parse_fms(const std::uint32_t eax) noexcept {
  m_stepping = eax & 0xF;
  const std::uint32_t base_model = (eax >> 4) & 0xF;
  const std::uint32_t base_family = (eax >> 8) & 0xF;
  m_processor_type = (eax >> 12) & 0x3;
  const std::uint32_t ext_model = (eax >> 16) & 0xF;
  const std::uint32_t ext_family = (eax >> 20) & 0xF;

  if (base_model == 0xF) {
    m_family = base_family + ext_family;
    m_model = base_model | (ext_model << 4);
  } else if (base_family == 0x6) {
    m_family = base_family;
    m_model = base_model | (ext_model << 4);
  } else {
    m_family = base_family;
    m_model = base_model;
  }
}

void CpuInfo::parse_frequencies() noexcept {
  if (m_max_leaf >= 0x16) {
    const auto [eax, ebx, ecx, edx] = query(0x16);
    m_base_mhz = eax & 0xFFFF;
    m_max_mhz = ebx & 0xFFFF;
    m_bus_mhz = ecx & 0xFFFF;
  }
}

void CpuInfo::initialize() noexcept {
  const auto r0 = query(0);
  m_max_leaf = r0.eax;

  __builtin_memcpy(m_vendor_string.data(), &r0.ebx, 4);
  __builtin_memcpy(m_vendor_string.data() + 4, &r0.edx, 4);
  __builtin_memcpy(m_vendor_string.data() + 8, &r0.ecx, 4);

  std::string_view vendor_sv{m_vendor_string.data()};
  if (vendor_sv == "GenuineIntel") {
    m_vendor = Vendor::Intel;
  } else if (vendor_sv == "AuthenticAMD") {
    m_vendor = Vendor::Amd;
  }

  const auto rx = query(0x80000000);
  m_max_ext_leaf = rx.eax;

  if (m_max_leaf >= 1) {
    const auto r = query(1);
    m_leaves[0] = {r.eax, r.ebx, r.ecx, r.edx};
    m_is_virtualized = (r.ecx & (1u << 31)) != 0;
    m_apic_id = (r.ebx >> 24) & 0xFF;
    m_clflush_size = ((r.ebx >> 8) & 0xFF) * 8;
    parse_fms(r.eax);
  }

  if (m_max_leaf >= 7) {
    auto r = query(7, 0);
    m_leaves[1] = {r.eax, r.ebx, r.ecx, r.edx};
    r = query(7, 1);
    m_leaves[2] = {r.eax, r.ebx, r.ecx, r.edx};
  }

  if (m_max_ext_leaf >= 0x80000001) {
    const auto [eax, ebx, ecx, edx] = query(0x80000001);
    m_leaves[3] = {eax, ebx, ecx, edx};
  }

  if (m_max_ext_leaf >= 0x80000008) {
    const auto [eax, ebx, ecx, edx] = query(0x80000008);
    m_leaves[4] = {eax, ebx, ecx, edx};
    m_phys_addr_bits = eax & 0xFF;
    m_virt_addr_bits = (eax >> 8) & 0xFF;

    if (m_vendor == Vendor::Amd) {
      m_apic_id_core_id_size = (ecx >> 12) & 0xF;
    }
  }

  if (m_vendor == Vendor::Intel && m_max_leaf >= 0xB) {
    std::uint32_t topo_leaf = (m_max_leaf >= 0x1F) ? 0x1F : 0xB;
    auto [_, _, _, edx] = query(topo_leaf, 0);
    m_apic_id = edx;
  }

  parse_brand();
  parse_frequencies();
  parse_caches();
  parse_tlb();
  parse_topology();
  parse_hypervisor();
}

namespace {
utils::TicketSpinlock cpu_profile_lock;
}

std::uint64_t CpuProfileManager::generate_fingerprint() noexcept {
  auto r = CpuInfo::query(1);
  const std::uint64_t fms = r.eax;

  r = CpuInfo::query(0);
  std::uint64_t core_type = 0;

  if (r.eax >= 0x1A) {
    r = CpuInfo::query(0x1A);
    core_type = r.eax >> 24;
  }

  return (core_type << 32) | fms;
}

const CpuInfo *CpuProfileManager::register_cpu() noexcept {
  const std::uint64_t fingerprint = generate_fingerprint();

  const std::size_t active = m_active_profiles.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < active; ++i) {
    if (m_profiles[i].fingerprint.load(std::memory_order_relaxed) == fingerprint) {
      return &m_profiles[i].info;
    }
  }

  {
    utils::IrqSaveGuard guard(cpu_profile_lock);

    const std::size_t curr_active = m_active_profiles.load(std::memory_order_relaxed);

    const std::size_t new_idx = curr_active;
    if (new_idx < MAX_ARCH_PROFILES) {
      m_profiles[new_idx].info.initialize();
      m_profiles[new_idx].fingerprint.store(fingerprint, std::memory_order_relaxed);
      m_active_profiles.store(new_idx + 1, std::memory_order_release);

      return &m_profiles[new_idx].info;
    }
  }

  return &m_profiles[0].info;
}

const CpuInfo *CpuProfileManager::get_current() const noexcept {
  const std::uint64_t fingerprint = generate_fingerprint();
  const std::size_t active = m_active_profiles.load(std::memory_order_relaxed);

  for (std::size_t i = 0; i < active; ++i) {
    if (m_profiles[i].fingerprint.load(std::memory_order_relaxed) == fingerprint) {
      return &m_profiles[i].info;
    }
  }

  return nullptr;
}
} // namespace kernel::hw