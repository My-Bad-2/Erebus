#include "hal/fpu.hpp"

#include <algorithm>

#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "utils/logger.hpp"
#include "utils/maths.hpp"

namespace kernel::hw::fpu {
namespace {
// Bit 63 of XCOMP_BV: the area is in the compacted format.
constexpr std::uint64_t XCOMP_BV_COMPACTED = 1ul << 63;

struct AreaLayout {
  std::uint64_t mask;
  std::size_t size;
};

// What the BSP worked out in initialize(); read-only from then on. Every CPU uses the same
// layout, because threads move between them and because the static calls are patched only once.
constinit AreaLayout g_plain{.mask = 0, .size = XSAVE_HEADER_OFFSET};
constinit AreaLayout g_amx{.mask = 0, .size = 0};
constinit bool g_compacted = false;

[[nodiscard]] constexpr std::uint32_t low_half(const std::uint64_t mask) noexcept {
  return static_cast<std::uint32_t>(mask);
}

[[nodiscard]] constexpr std::uint32_t high_half(const std::uint64_t mask) noexcept {
  return static_cast<std::uint32_t>(mask >> 32);
}

// Adds up the components of @p mask the way the CPU lays them out.
[[nodiscard]] std::size_t measure_area(const std::uint64_t mask, const bool compacted) noexcept {
  constexpr std::uint32_t ALIGN_TO_64 = 1u << 1;

  std::size_t size = XSAVE_HEADER_OFFSET + sizeof(XSaveHeader);

  // x87 and SSE, components 0 and 1, live in the legacy region.
  for (std::uint32_t component = 2; component < 64; ++component) {
    if (((mask >> component) & 1) == 0) {
      continue;
    }

    const auto info = CpuInfo::query(0xD, component);
    if (!compacted) {
      size = std::max<std::size_t>(size, info.ebx + info.eax);
      continue;
    }

    if ((info.ecx & ALIGN_TO_64) != 0) {
      size = utils::maths::align_up(size, AREA_ALIGNMENT);
    }

    size += info.eax;
  }

  return size;
}

// Whether IA32_XFD can keep tile data away from the threads that did not ask for it. Without that
// it would be enabled for everyone and saved for no one.
[[nodiscard]] bool can_fence_tile_data(const CpuInfo* cpu) noexcept {
  constexpr std::uint32_t TILE_DATA_COMPONENT = 18;
  constexpr std::uint32_t SUPPORTS_XFD = 1u << 2;

  return cpu->has<Feature::AMX_TILE>() && cpu->has<Feature::XFD>() &&
         (CpuInfo::query(0xD, TILE_DATA_COMPONENT).ecx & SUPPORTS_XFD) != 0;
}
}  // namespace

namespace detail {
/// @brief Saves the standard 512-byte state (x87, MMX, SSE). The mask has no say in it.
void cpu_fxsave(void* buffer, std::uint64_t) noexcept {
  asm volatile("fxsave64 %0"
               : "=m"(*static_cast<std::uint8_t*>(buffer))
               : /* No Input */
               : "memory");
}

/// @brief Restores the standard 512-byte state.
void cpu_fxrestore(const void* buffer, std::uint64_t) noexcept {
  asm volatile("fxrstor64 %0"
               : /* No Output */
               : "m"(*static_cast<const std::uint8_t*>(buffer))
               : "memory");
}

/// @brief Standard format, every component.
void cpu_xsave(void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xsave64 %0"
               : "=m"(*static_cast<std::uint8_t*>(buffer))
               : "a"(low_half(mask)), "d"(high_half(mask))
               : "memory");
}

/// @brief Standard format, skipping what has not changed since the last XRSTOR from this area.
void cpu_xsaveopt(void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xsaveopt64 %0"
               : "=m"(*static_cast<std::uint8_t*>(buffer))
               : "a"(low_half(mask)), "d"(high_half(mask))
               : "memory");
}

/// @brief Compacted format, skipping components in their initial state.
void cpu_xsavec(void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xsavec64 %0"
               : "=m"(*static_cast<std::uint8_t*>(buffer))
               : "a"(low_half(mask)), "d"(high_half(mask))
               : "memory");
}

/// @brief Restores either format; the area's header says which one it is in.
void cpu_xrstor(const void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xrstor64 %0"
               : /* No Output */
               : "m"(*static_cast<const std::uint8_t*>(buffer)), "a"(low_half(mask)),
                 "d"(high_half(mask))
               : "memory");
}

/// @brief Compacted format, with every optimization there is, and able to save supervisor state.
void cpu_xsaves(void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xsaves64 %0"
               : "=m"(*static_cast<std::uint8_t*>(buffer))
               : "a"(low_half(mask)), "d"(high_half(mask))
               : "memory");
}

/// @brief Restores the compacted format, supervisor state included.
void cpu_xrstors(const void* buffer, const std::uint64_t mask) noexcept {
  asm volatile("xrstors64 %0"
               : /* No Output */
               : "m"(*static_cast<const std::uint8_t*>(buffer)), "a"(low_half(mask)),
                 "d"(high_half(mask))
               : "memory");
}
}  // namespace detail

DEFINE_STATIC_CALL(save, detail::cpu_fxsave);
REGISTER_STATIC_CALL_ALT_PRIO(save, detail::cpu_xsave, 100, Feature::XSAVE);
REGISTER_STATIC_CALL_ALT_PRIO(save, detail::cpu_xsaveopt, 200, Feature::XSAVEOPT);
REGISTER_STATIC_CALL_ALT_PRIO(save, detail::cpu_xsavec, 300, Feature::XSAVEC);

DEFINE_STATIC_CALL(saves, detail::cpu_fxsave);
REGISTER_STATIC_CALL_ALT_PRIO(saves, detail::cpu_xsave, 100, Feature::XSAVE);
REGISTER_STATIC_CALL_ALT_PRIO(saves, detail::cpu_xsaveopt, 200, Feature::XSAVEOPT);
REGISTER_STATIC_CALL_ALT_PRIO(saves, detail::cpu_xsavec, 300, Feature::XSAVEC);
REGISTER_STATIC_CALL_ALT_PRIO(saves, detail::cpu_xsaves, 400, Feature::XSS);

DEFINE_STATIC_CALL(restore, detail::cpu_fxrestore);
REGISTER_STATIC_CALL_ALT_PRIO(restore, detail::cpu_xrstor, 100, Feature::XSAVE);

DEFINE_STATIC_CALL(restores, detail::cpu_fxrestore);
REGISTER_STATIC_CALL_ALT_PRIO(restores, detail::cpu_xrstor, 100, Feature::XSAVE);
REGISTER_STATIC_CALL_ALT_PRIO(restores, detail::cpu_xrstors, 200, Feature::XSS);

void initialize() noexcept {
  const CpuInfo* cpu = percpu::self()->info;

  // TS is off too: nothing here hands out the FPU lazily, so #NM would only ever be a bug.
  write::cr0(read::cr0().without_em().without_ts().with_mp().with_ne());

  CR4 cr4 = read::cr4().with_osfxsr().with_osxmmexcpt();
  cr4.set_osxsave(cpu->has<Feature::XSAVE>());
  write::cr4(cr4);

  // Nothing promises what the firmware left in here. Mask all SIMD exceptions, set rounding to
  // the nearest.
  const std::uint32_t default_mxcsr = DEFAULT_MXCSR;
  asm volatile(
      "fninit\n\t"
      "ldmxcsr %0"
      : /* No Output */
      : "m"(default_mxcsr)
      : /* No Clobber */);

  if (!cpu->has<Feature::XSAVE>()) {
    if (percpu::id() == 0) {
      utils::logger::info("FPU: no XSAVE, {}-byte save areas (FXSAVE)\n", g_plain.size);
    }

    return;
  }

  const auto leaf0 = CpuInfo::query(0xD, 0);
  const std::uint64_t supported_xcr0 = (static_cast<std::uint64_t>(leaf0.edx) << 32) | leaf0.eax;

  // Built from nothing, rather than on top of what the firmware left behind: the size of every
  // save area depends on this. PKRU stays out of it, as Thread switches it with WRPKRU.
  XCR0 wanted = XCR0{}.with_x87().with_sse().with_avx().with_opmask().with_zmm_hi256();
  wanted.set_hi16_zmm();

  const bool amx = can_fence_tile_data(cpu);
  wanted.set_xtilecfg(amx);
  wanted.set_xtiledata(amx);

  const XCR0 xcr0{wanted.raw() & supported_xcr0};
  write::xcr0(xcr0);

  if ((xcr0.raw() & XCR0::XTILEDATA) != 0) {
    write::msr(MSR_IA32_XFD, XFD_NO_AMX.raw());
  }

  if (percpu::id() == 0) {
    g_compacted = cpu->has<Feature::XSS>() || cpu->has<Feature::XSAVEC>();

    const std::uint64_t plain = xcr0.raw() & XCR0::STANDARD_MASK;
    g_plain = {.mask = plain, .size = measure_area(plain, g_compacted)};

    if ((xcr0.raw() & XCR0::XTILEDATA) != 0) {
      const std::uint64_t all = xcr0.raw() & XCR0::STANDARD_AMX_MASK;
      g_amx = {.mask = all, .size = measure_area(all, g_compacted)};
    }

    utils::logger::info("FPU: XCR0 {:#x}, {}-byte save areas ({})\n", xcr0.raw(), g_plain.size,
                        g_compacted ? "compacted" : "standard");
  }

  if (!cpu->has<Feature::XSS>()) {
    return;
  }

  const auto leaf1 = CpuInfo::query(0xD, 1);
  const std::uint64_t supported_xss = (static_cast<std::uint64_t>(leaf1.edx) << 32) | leaf1.ecx;

  // Nothing saves these yet: no state mask contains them, and they take no room in an area
  // until one does.
  const Ia32Xss wanted_xss = Ia32Xss{read::msr(MSR_IA32_XSS)}.with_pt().with_cet_u().with_cet_s();
  write::msr(MSR_IA32_XSS, wanted_xss.raw() & supported_xss);
}

std::uint64_t state_mask(const bool amx) noexcept {
  return amx && amx_available() ? g_amx.mask : g_plain.mask;
}

std::size_t area_size(const bool amx) noexcept {
  return amx && amx_available() ? g_amx.size : g_plain.size;
}

bool amx_available() noexcept { return g_amx.mask != 0; }

void init_area(void* area, const std::uint64_t mask) noexcept {
  *static_cast<LegacyArea*>(area) = LegacyArea{};
  set_area_mask(area, mask);
}

void set_area_mask(void* area, const std::uint64_t mask) noexcept {
  // FXSAVE has no header, and an XSTATE_BV of zero already means "all in the initial state".
  if (g_plain.mask == 0) {
    return;
  }

  auto* header =
      reinterpret_cast<XSaveHeader*>(static_cast<std::byte*>(area) + XSAVE_HEADER_OFFSET);
  header->xcomp_bv = g_compacted ? XCOMP_BV_COMPACTED | mask : 0;
}
}  // namespace kernel::hw::fpu
