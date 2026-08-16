#pragma once

#include <array>
#include <atomic>
#include <cpuid.h>
#include <cstdint>
#include <span>
#include <string_view>

namespace kernel::hw {
enum class Vendor : std::uint8_t { Unknown, Intel, Amd };
enum class Hypervisor : std::uint8_t { None, Unknown, Kvm, Vmware, HyperV, Xen, Parallels, VirtualBox, Qemu, Bhyve };
enum class CoreType : std::uint8_t { Unknown = 0, Atom = 0x20, Core = 0x40 };
enum class CacheType : std::uint8_t { Null = 0, Data = 1, Instruction = 2, Unified = 3 };
enum class PageSize : std::uint8_t { Size4K, Size2M, Size4M, Size1G, Unknown };
enum class TopoLevel : std::uint8_t { Invalid = 0, SMT = 1, Core = 2, Module = 3, Tile = 4, Die = 5, Package = 6 };
enum class Reg : std::uint32_t { EAX = 0, EBX = 1, ECX = 2, EDX = 3 };

struct CacheInfo {
  std::uint32_t level;
  CacheType type;
  std::uint32_t size_bytes;
  std::uint32_t ways;
  std::uint32_t line_size;
  std::uint32_t sets;
};

struct TlbInfo {
  CacheType type;
  PageSize page_size;
  std::uint32_t entries;
  std::uint32_t ways; // 0xFF = Fully Associative
};

struct TopologyInfo {
  TopoLevel level_type;
  std::uint32_t logical_processors;
  std::uint32_t id_shift;
};

consteval std::uint32_t make_feat(const std::uint32_t leaf, const std::uint32_t subleaf, const Reg reg,
                                  const std::uint32_t bit) {
  const std::uint32_t is_ext = (leaf & 0x80000000) ? 1 : 0;
  const std::uint32_t base_id = leaf & 0xff;
  return bit | (static_cast<std::uint32_t>(reg) << 5) | (subleaf << 7) | (base_id << 15) | (is_ext << 23);
}

constexpr std::uint32_t map_leaf_to_index(const std::uint32_t leaf, const std::uint32_t subleaf) {
  if (leaf == 1 && subleaf == 0) {
    return 0;
  }

  if (leaf == 7 && subleaf == 0) {
    return 1;
  }

  if (leaf == 7 && subleaf == 1) {
    return 2;
  }

  if (leaf == 0x80000001 && subleaf == 0) {
    return 3;
  }

  if (leaf == 0x80000008 && subleaf == 0) {
    return 4;
  }

  return 0xFFFFFFFF; // Leaf is not pre-cached
}

enum class Feature : std::uint32_t {
  // Base Features: Leaf 1
  SSE3 = make_feat(1, 0, Reg::ECX, 0),
  PCLMULQDQ = make_feat(1, 0, Reg::ECX, 1),
  MONITOR = make_feat(1, 0, Reg::ECX, 3),
  SSSE3 = make_feat(1, 0, Reg::ECX, 9),
  FMA = make_feat(1, 0, Reg::ECX, 12),
  PCID = make_feat(1, 0, Reg::ECX, 17),
  SSE4_1 = make_feat(1, 0, Reg::ECX, 19),
  SSE4_2 = make_feat(1, 0, Reg::ECX, 20),
  X2APIC = make_feat(1, 0, Reg::ECX, 21),
  POPCNT = make_feat(1, 0, Reg::ECX, 23),
  AES = make_feat(1, 0, Reg::ECX, 25),
  XSAVE = make_feat(1, 0, Reg::ECX, 26),
  OSXSAVE = make_feat(1, 0, Reg::ECX, 27),
  AVX = make_feat(1, 0, Reg::ECX, 28),
  F16C = make_feat(1, 0, Reg::ECX, 29),
  RDRAND = make_feat(1, 0, Reg::ECX, 30),
  HYPERVISOR = make_feat(1, 0, Reg::ECX, 31),

  FPU = make_feat(1, 0, Reg::EDX, 0),
  PSE = make_feat(1, 0, Reg::EDX, 3),
  TSC = make_feat(1, 0, Reg::EDX, 4),
  MSR = make_feat(1, 0, Reg::EDX, 5),
  APIC = make_feat(1, 0, Reg::EDX, 9),
  SEP = make_feat(1, 0, Reg::EDX, 11),
  PGE = make_feat(1, 0, Reg::EDX, 13),
  CMOV = make_feat(1, 0, Reg::EDX, 15),
  PAT = make_feat(1, 0, Reg::EDX, 16),
  CLFLUSH = make_feat(1, 0, Reg::EDX, 19),
  ACPI = make_feat(1, 0, Reg::EDX, 22),
  SSE = make_feat(1, 0, Reg::EDX, 25),
  SSE2 = make_feat(1, 0, Reg::EDX, 26),
  HTT = make_feat(1, 0, Reg::EDX, 28),

  // MONITOR/MWAIT Features: Leaf 5, Subleaf 0
  IBE = make_feat(5, 0, Reg::ECX, 1),

  // Extended Features: Leaf 7, Subleaf 0
  FSGSBASE = make_feat(7, 0, Reg::EBX, 0),
  SGX = make_feat(7, 0, Reg::EBX, 2),
  BMI1 = make_feat(7, 0, Reg::EBX, 3),
  AVX2 = make_feat(7, 0, Reg::EBX, 5),
  SMEP = make_feat(7, 0, Reg::EBX, 7),
  BMI2 = make_feat(7, 0, Reg::EBX, 8),
  ERMS = make_feat(7, 0, Reg::EBX, 9),
  INVPCID = make_feat(7, 0, Reg::EBX, 10),
  AVX512F = make_feat(7, 0, Reg::EBX, 16),
  RDSEED = make_feat(7, 0, Reg::EBX, 18),
  SMAP = make_feat(7, 0, Reg::EBX, 20),
  CLFLUSHOPT = make_feat(7, 0, Reg::EBX, 23),
  SHA = make_feat(7, 0, Reg::EBX, 29),
  AVX512BW = make_feat(7, 0, Reg::EBX, 30),

  UMIP = make_feat(7, 0, Reg::ECX, 2),
  PKU = make_feat(7, 0, Reg::ECX, 3),
  WAITPKG = make_feat(7, 0, Reg::ECX, 5),
  CET_SHSTK = make_feat(7, 0, Reg::ECX, 7),
  GNFI = make_feat(7, 0, Reg::ECX, 8),
  VAES = make_feat(7, 0, Reg::ECX, 9),
  AVX512_VNNI = make_feat(7, 0, Reg::ECX, 11),

  HYBRID = make_feat(7, 0, Reg::EDX, 15),
  CET_IBT = make_feat(7, 0, Reg::EDX, 20),
  AMX_BF16 = make_feat(7, 0, Reg::EDX, 22),
  AMX_TILE = make_feat(7, 0, Reg::EDX, 24),

  // Extended Features: Leaf 7, Subleaf 1
  LAM = make_feat(7, 1, Reg::EAX, 26),
  AVX10 = make_feat(7, 1, Reg::EDX, 19),
  APX_F = make_feat(7, 1, Reg::EDX, 21),

  // AMD/Intel Extended Features: Leaf 0x80000001
  SVM = make_feat(0x80000001, 0, Reg::ECX, 2),
  LZCNT = make_feat(0x80000001, 0, Reg::ECX, 5),
  MONITORX = make_feat(0x80000001, 0, Reg::ECX, 29),

  SYSCALL = make_feat(0x80000001, 0, Reg::EDX, 11),
  NX = make_feat(0x80000001, 0, Reg::EDX, 20),
  GIB = make_feat(0x80000001, 0, Reg::EDX, 26),
  RDTSCP = make_feat(0x80000001, 0, Reg::EDX, 27),
  LM = make_feat(0x80000001, 0, Reg::EDX, 29)
};

class CpuInfo {
  friend class CpuProfileManager;
  struct Registers {
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
  };

  Vendor m_vendor = Vendor::Unknown;
  Hypervisor m_hypervisor = Hypervisor::None;
  bool m_is_virtualized{false};

  std::uint8_t m_stepping{0};

  std::uint32_t m_apic_id{0};
  std::uint32_t m_clflush_size{0};
  std::uint32_t m_phys_addr_bits{0};
  std::uint32_t m_virt_addr_bits{0};

  std::uint32_t m_family{0};
  std::uint32_t m_model{0};
  std::uint32_t m_processor_type{0};

  std::uint32_t m_base_mhz{0};
  std::uint32_t m_max_mhz{0};
  std::uint32_t m_bus_mhz{0};

  std::array<char, 13> m_vendor_string{};
  std::array<char, 49> m_brand_string{};

  std::size_t m_num_caches{0};
  std::array<CacheInfo, 16> m_caches{};

  std::size_t m_num_tlbs{0};
  std::array<TlbInfo, 16> m_tlbs{};

  std::size_t m_num_topology{0};
  std::array<TopologyInfo, 8> m_topology{};

  alignas(64) std::array<std::array<std::uint32_t, 4>, 5> m_leaves{};

  static Registers query(const std::uint32_t leaf, const std::uint32_t subleaf = 0) noexcept {
    Registers r;
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
    return r;
  }

  void parse_fms(std::uint32_t eax) noexcept;
  void parse_frequencies(std::uint32_t max_leaf) noexcept;
  void parse_brand(std::uint32_t max_ext_leaf) noexcept;
  void parse_caches(std::uint32_t max_leaf, std::uint32_t max_ext_leaf) noexcept;
  void parse_tlb(std::uint32_t max_leaf, std::uint32_t max_ext_leaf) noexcept;
  void parse_topology(std::uint32_t max_leaf, std::uint32_t max_ext_leaf) noexcept;
  void parse_hypervisor() noexcept;

public:
  [[nodiscard]] Vendor vendor() const noexcept { return m_vendor; }
  [[nodiscard]] Hypervisor hypervisor() const noexcept { return m_hypervisor; }
  [[nodiscard]] std::string_view vendor_string() const noexcept { return {m_vendor_string.data()}; }
  [[nodiscard]] std::string_view brand_string() const noexcept { return {m_brand_string.data()}; }

  [[nodiscard]] bool is_virtualized() const noexcept { return m_is_virtualized; }
  [[nodiscard]] std::uint32_t apic_id() const noexcept { return m_apic_id; }
  [[nodiscard]] std::uint32_t clflush_size() const noexcept { return m_clflush_size; }
  [[nodiscard]] std::uint32_t phys_addr_bits() const noexcept { return m_phys_addr_bits; }
  [[nodiscard]] std::uint32_t virt_addr_bits() const noexcept { return m_virt_addr_bits; }

  [[nodiscard]] std::uint32_t family() const noexcept { return m_family; }
  [[nodiscard]] std::uint32_t model() const noexcept { return m_model; }
  [[nodiscard]] std::uint32_t stepping() const noexcept { return m_stepping; }
  [[nodiscard]] std::uint32_t processor_type() const noexcept { return m_processor_type; }

  [[nodiscard]] std::uint32_t base_mhz() const noexcept { return m_base_mhz; }
  [[nodiscard]] std::uint32_t max_mhz() const noexcept { return m_max_mhz; }
  [[nodiscard]] std::uint32_t bus_mhz() const noexcept { return m_bus_mhz; }

  [[nodiscard]] std::span<const CacheInfo> caches() const noexcept { return {m_caches.data(), m_num_caches}; }
  [[nodiscard]] std::span<const TlbInfo> tlbs() const noexcept { return {m_tlbs.data(), m_num_tlbs}; }
  [[nodiscard]] std::span<const TopologyInfo> topology() const noexcept { return {m_topology.data(), m_num_topology}; }

  template <Feature F> [[nodiscard]] constexpr bool has() const noexcept {
    constexpr auto val = static_cast<std::uint32_t>(F);
    constexpr std::uint32_t leaf_base = (val >> 15) & 0xFF;
    constexpr std::uint32_t is_ext = (val >> 23) & 0x1;
    constexpr std::uint32_t leaf = is_ext ? (leaf_base | 0x80000000) : leaf_base;
    constexpr std::uint32_t subleaf = (val >> 7) & 0xFF;

    constexpr std::uint32_t bit = val & 0x1F;
    constexpr std::uint32_t reg = (val >> 5) & 0x3;
    constexpr std::uint32_t cache_idx = map_leaf_to_index(leaf, subleaf);

    if constexpr (cache_idx != 0xFFFFFFFF) {
      return (m_leaves[cache_idx][reg] & (1u << bit)) != 0;
    } else {
      auto r = query(leaf, subleaf);

      if constexpr (reg == static_cast<uint32_t>(Reg::EAX)) {
        return (r.eax & (1u << bit)) != 0;
      } else if constexpr (reg == static_cast<uint32_t>(Reg::EBX)) {
        return (r.ebx & (1u << bit)) != 0;
      } else if constexpr (reg == static_cast<uint32_t>(Reg::ECX)) {
        return (r.ecx & (1u << bit)) != 0;
      } else {
        return (r.edx & (1u << bit)) != 0;
      }
    }
  }

  [[nodiscard]] constexpr bool has(Feature feat) const noexcept {
    const auto val = static_cast<std::uint32_t>(feat);
    const std::uint32_t leaf_base = (val >> 15) & 0xFF;
    const std::uint32_t is_ext = (val >> 23) & 0x1;
    const std::uint32_t leaf = is_ext ? (leaf_base | 0x80000000) : leaf_base;
    const std::uint32_t subleaf = (val >> 7) & 0xFF;

    const std::uint32_t bit = val & 0x1F;
    const std::uint32_t reg = (val >> 5) & 0x3;
    const std::uint32_t cache_idx = map_leaf_to_index(leaf, subleaf);

    if (cache_idx != 0xFFFFFFFF) [[likely]] {
      return (m_leaves[cache_idx][reg] & (1u << bit)) != 0;
    }

    const auto r = query(leaf, subleaf);

    if (reg == static_cast<uint32_t>(Reg::EAX)) {
      return (r.eax & (1u << bit)) != 0;
    }

    if (reg == static_cast<uint32_t>(Reg::EBX)) {
      return (r.ebx & (1u << bit)) != 0;
    }

    if (reg == static_cast<uint32_t>(Reg::ECX)) {
      return (r.ecx & (1u << bit)) != 0;
    }

    return (r.edx & (1u << bit)) != 0;
  }

  void initialize() noexcept;
};

// Hybrid systems have 2 architectures (P- and E- cores)
inline constexpr std::size_t MAX_ARCH_PROFILES = 2;
class CpuProfileManager {
  struct ProfileEntry {
    std::atomic<std::uint64_t> fingerprint{0};
    CpuInfo info;
  };

  std::array<ProfileEntry, MAX_ARCH_PROFILES> m_profiles;
  std::atomic<std::size_t> m_active_profiles{0};

  static std::uint64_t generate_fingerprint() noexcept;

public:
  const CpuInfo *register_cpu() noexcept;
  [[nodiscard]] const CpuInfo *get_current() const noexcept;
};

inline CpuProfileManager profile_manager;
} // namespace kernel::hw