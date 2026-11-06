/// @file fpu.hpp
/// @brief The FPU and its descendants, SSE to AMX: turning them on, and saving their state.
///
/// The kernel is built with -mgeneral-regs-only, so outside a utils::FpuGuard it never touches
/// these registers, and what they hold belongs to the running thread. A thread's copy of them
/// lives in a save *area*, in the format of whichever instruction `saves` got patched to:
///
///  - XSAVES or XSAVEC write the compacted format, where only the components being saved take
///    up room.
///  - XSAVE and XSAVEOPT write the standard format, where CPUID fixes the offset of every
///    component. Intel keeps a hole for MPX in there and AMD does not.
///  - FXSAVE, on CPUs without any of those, writes just the 512-byte legacy region.
///
/// So neither the size nor the layout of an area can be a constant. initialize() works them out
/// on the BSP, and area_size() and init_area() are how the rest of the kernel gets at them.
///
/// AMX is the exception to everything: its 8 KiB of tile data would make every area huge for
/// the sake of very few threads, so its components are saved only for threads that asked for
/// them. Everyone else runs with them disabled through IA32_XFD.
#pragma once

#include <array>
#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>

#include "patch/static_call.hpp"

namespace kernel::hw::fpu {
inline constexpr std::uint32_t MSR_IA32_XFD = 0x1C4;
inline constexpr std::uint32_t MSR_IA32_XSS = 0xDA0;

/// @brief x87 control word after FNINIT: every exception masked, 64-bit precision.
inline constexpr std::uint16_t DEFAULT_FCW = 0x037F;
/// @brief MXCSR after reset: every exception masked, round to nearest.
inline constexpr std::uint32_t DEFAULT_MXCSR = 0x1F80;

/// @brief Alignment every save area needs: 64 for the XSAVE family, 16 for FXSAVE.
inline constexpr std::size_t AREA_ALIGNMENT = 64;

class Ia32Xfd {
  std::uint64_t m_data{};

 public:
  constexpr explicit Ia32Xfd(const std::uint64_t raw = 0) noexcept : m_data(raw) {}
  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(xtilecfg, 17)
  BF_BIT_RW(xtiledata, 18)
};

/// @brief IA32_XFD of a thread without AMX: touching the tile registers raises #NM.
inline constexpr Ia32Xfd XFD_NO_AMX = Ia32Xfd{}.with_xtilecfg().with_xtiledata();

class Ia32XfdErr {
  std::uint64_t m_data{};

 public:
  constexpr explicit Ia32XfdErr(const std::uint64_t raw = 0) noexcept : m_data(raw) {}

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(xtilecfg, 17)
  BF_BIT_RW(xtiledata, 18)
};

class Ia32Xss {
  std::uint64_t m_data{};

 public:
  constexpr explicit Ia32Xss(const std::uint64_t raw = 0) noexcept : m_data(raw) {}
  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_BIT_RW(pt, 8)
  BF_BIT_RW(pasid, 10)
  BF_BIT_RW(cet_u, 11)
  BF_BIT_RW(cet_s, 12)
  BF_BIT_RW(hdc, 13)
  BF_BIT_RW(uintr, 14)
  BF_BIT_RW(lbr, 15)
  BF_BIT_RW(hwp, 16)
};

/// @brief The part of the legacy region that the kernel has to fill in itself.
struct LegacyArea {
  std::uint16_t fcw{DEFAULT_FCW};
  std::array<std::uint8_t, 22> unused{};
  std::uint32_t mxcsr{DEFAULT_MXCSR};
};

static_assert(offsetof(LegacyArea, mxcsr) == 24);

/// @brief 64-byte XSAVE Header located at byte offset 512 of an XSAVE area.
struct alignas(64) XSaveHeader {
  std::uint64_t xstate_bv{0};  ///< Bitmask of state components currently holding non-initial state
  std::uint64_t xcomp_bv{0};   ///< Bit 63 = Compacted format; bits [62:0] = components in layout
  std::array<std::uint64_t, 6> reserved{};
};

static_assert(sizeof(XSaveHeader) == 64, "XSaveHeader must be exactly 64 bytes");
inline constexpr std::size_t XSAVE_HEADER_OFFSET = 512;

/// @brief Sets the calling CPU up for FPU use, with its registers in their initial state. Every
///        CPU runs it once, the BSP first: the BSP also works out what a save area looks like.
void initialize() noexcept;

/// @brief The state components of a thread, for the `mask` of the save calls. Only what is
///        enabled in XCR0: 0 on a CPU without XSAVE.
/// @param amx Whether the thread uses AMX. Without AMX the answer is the same either way.
[[nodiscard]] std::uint64_t state_mask(bool amx = false) noexcept;

/// @brief Size of a save area for state_mask(@p amx).
[[nodiscard]] std::size_t area_size(bool amx = false) noexcept;

/// @brief Whether the CPU has AMX and it is enabled, with IA32_XFD to keep it out of the way.
[[nodiscard]] bool amx_available() noexcept;

/// @brief Turns zeroed memory into a save area holding the initial state of @p mask, ready to be
///        restored.
/// @pre @p area is 64-byte aligned, and area_size() bytes of it are zero.
void init_area(void* area, std::uint64_t mask) noexcept;

/// @brief Makes the area at @p area describe @p mask instead of the components it was built for.
///
/// For extending an area into a larger buffer. The components already in it stay where they are,
/// which holds as long as the new ones number higher, as AMX's do.
void set_area_mask(void* area, std::uint64_t mask) noexcept;

/// @brief Saves the components in @p mask to @p buffer, as far as they are user state.
DECLARE_STATIC_CALL(void, save, void* buffer, std::uint64_t mask);

/// @brief Restores the components in @p mask from @p buffer, as far as they are user state.
DECLARE_STATIC_CALL(void, restore, const void* buffer, std::uint64_t mask);

/// @brief Saves the components in @p mask to @p buffer, in the area format described above.
DECLARE_STATIC_CALL(void, saves, void* buffer, std::uint64_t mask);

/// @brief Restores the components in @p mask from an area in the format described above.
DECLARE_STATIC_CALL(void, restores, const void* buffer, std::uint64_t mask);
}  // namespace kernel::hw::fpu
