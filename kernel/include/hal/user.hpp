/// @file user.hpp
/// @brief Getting into user mode, and the state a user thread owns on the CPU.
///
/// The only way into ring 3 is a return from an interrupt, real or made up: enter() builds the
/// frame a return to user mode would find and takes it, and resume() does the same with every
/// register user code had somewhere else, which is how a forked thread carries on where the one it
/// was copied from was. Everything after that comes back into the
/// kernel through the interrupt entry (see hal/interrupts), which swaps GS on the way in and out
/// whenever the interrupted code was in ring 3.
///
/// While the kernel runs, IA32_KERNEL_GS_BASE holds the GS base of the user thread, as SWAPGS
/// left it, and the FS base and PKRU are the user's too. With CR4.FSGSBASE on, user code can change
/// both bases without asking, and it can always change PKRU, so a switch away from a user thread
/// reads them back (see sched::switch_to()).
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal/interrupts/interrupts.hpp"
#include "hal/smp/gdt.hpp"
#include "memory/address.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/pagemap/features.hpp"

namespace kernel::hw::user {
/// @brief Selectors of ring 3, with RPL 3 as IRET wants them.
inline constexpr std::uint16_t CODE_SELECTOR =
    std::to_underlying(gdt::with_rpl(gdt::Selector::UserCode, gdt::PrivilegeLevel::Ring3));
inline constexpr std::uint16_t DATA_SELECTOR =
    std::to_underlying(gdt::with_rpl(gdt::Selector::UserData, gdt::PrivilegeLevel::Ring3));

/// @brief What user code starts with: interrupts on, and the bit that is always set.
inline constexpr std::uint64_t INITIAL_RFLAGS = 0x202;

/// @brief Where the canonical lower half ends: 2^47, or 2^56 with 5-level paging.
[[nodiscard]] inline std::uintptr_t lower_half_end() noexcept {
  const std::size_t bits = memory::PAGE_SHIFT_4KB + 9 * memory::vmm::g_paging_features.levels;
  return std::uintptr_t{1} << (bits - 1);
}

/// @brief The end of what user code may map: the lower half, less its last page.
///
/// Code in that last page could end with a SYSCALL whose return address is the first one past the
/// half, which is not canonical, and a SYSRET or IRET to it faults in ring 0 on Intel, with the
/// user's GS already loaded. Nothing can run there if nothing can be mapped there.
[[nodiscard]] inline std::uintptr_t user_space_end() noexcept {
  return lower_half_end() - memory::PAGE_SIZE;
}

/// @brief Whether @p addr is one user code may use: below user_space_end(), without page zero.
[[nodiscard]] inline bool is_user_address(const memory::VirtualAddress addr) noexcept {
  return addr.value() >= memory::PAGE_SIZE && addr.value() < user_space_end();
}

/// @brief Whether all of [@p addr, @p addr + @p size) is user memory. An empty range is, wherever
///        it is, as there is nothing in it to touch.
[[nodiscard]] inline bool is_user_range(const memory::VirtualAddress addr,
                                        const std::size_t size) noexcept {
  return size == 0 || (is_user_address(addr) && size <= user_space_end() - addr.value());
}

/// @brief Where user code is, all of it the kernel has to put back for it to carry on: its
///        registers, where it was, its stack pointer and its flags.
struct ResumeFrame {
  interrupts::RegisterContext regs;
  std::uint64_t rip;
  std::uint64_t rsp;
  std::uint64_t rflags;
};

/// @brief The flags of @p rflags user code may have, with interrupts on and the bit that is
///        always set: the arithmetic ones, TF, DF, AC and ID. Never IOPL, NT, RF or VM, which
///        a return from ring 0 would otherwise hand it.
[[nodiscard]] constexpr std::uint64_t user_rflags(const std::uint64_t rflags) noexcept {
  constexpr std::uint64_t USER_CHANGEABLE = 0x0000'0000'0024'0DD5;  // CF PF AF ZF SF TF DF OF AC ID
  return (rflags & USER_CHANGEABLE) | INITIAL_RFLAGS;
}

/// @brief Leaves the kernel for ring 3 at @p pc, with @p sp as the stack pointer and @p arg in
///        RDI. Every other register starts out as zero.
///
/// The calling thread's kernel stack is where the CPU comes back to on the next interrupt, at its
/// top, so whatever is on it now is gone for good.
/// @pre Interrupts are off, and GS is the kernel's. The thread is a user thread whose address
///      space is loaded, with its GS base in IA32_KERNEL_GS_BASE.
[[noreturn]] void enter(memory::VirtualAddress pc, memory::VirtualAddress sp,
                        std::uint64_t arg) noexcept;

/// @brief Leaves the kernel for ring 3 with everything in @p frame: every register, the place, the
///        stack pointer, and the flags, as user_rflags() leaves them.
///
/// Like enter(), whatever is on the calling thread's kernel stack is gone for good.
/// @pre As for enter(), and @p frame.rip and @p frame.rsp are canonical lower-half addresses: a
///      return to anything else would fault before ring 3, with the user's GS already loaded.
[[noreturn]] void resume(const ResumeFrame& frame) noexcept;

/// @brief Clears RFLAGS.AC, which user code may have set, and which turns SMAP off.
///
/// An interrupt from ring 3 does not clear it, and the kernel must never run with it set. Nothing
/// on CPUs without SMAP, where the flag means nothing to the kernel.
void clear_ac() noexcept;

/// @brief The GS base of the current user thread, from IA32_KERNEL_GS_BASE.
/// @pre In the kernel, with interrupts off or the thread otherwise unable to move.
[[nodiscard]] std::uint64_t read_gs_base() noexcept;

/// @brief Sets the GS base the next return to user mode swaps in.
void write_gs_base(std::uint64_t base) noexcept;

/// @brief Whether user code can change its own FS and GS bases, which CR4.FSGSBASE allows.
[[nodiscard]] bool can_change_bases() noexcept;

/// @brief The current PKRU, which user code can change with WRPKRU. Zero without PKU.
[[nodiscard]] std::uint32_t read_pkru() noexcept;
}  // namespace kernel::hw::user
