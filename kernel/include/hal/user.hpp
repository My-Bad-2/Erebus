/// @file user.hpp
/// @brief Getting into user mode, and the state a user thread owns on the CPU.
///
/// The only way into ring 3 is a return from an interrupt, real or made up: enter() builds the
/// frame a return to user mode would find and takes it. Everything after that comes back into the
/// kernel through the interrupt entry (see hal/interrupts), which swaps GS on the way in and out
/// whenever the interrupted code was in ring 3.
///
/// While the kernel runs, IA32_KERNEL_GS_BASE holds the GS base of the user thread, as SWAPGS
/// left it, and the FS base and PKRU are the user's too. With CR4.FSGSBASE on, user code can change
/// both bases without asking, and it can always change PKRU, so a switch away from a user thread
/// reads them back (see sched::switch_to()).
#pragma once

#include <cstdint>

#include "hal/smp/gdt.hpp"
#include "memory/address.hpp"
#include "memory/memory.hpp"

namespace kernel::hw::user {
/// @brief Selectors of ring 3, with RPL 3 as IRET wants them.
inline constexpr std::uint16_t CODE_SELECTOR =
    std::to_underlying(gdt::with_rpl(gdt::Selector::UserCode, gdt::PrivilegeLevel::Ring3));
inline constexpr std::uint16_t DATA_SELECTOR =
    std::to_underlying(gdt::with_rpl(gdt::Selector::UserData, gdt::PrivilegeLevel::Ring3));

/// @brief What user code starts with: interrupts on, and the bit that is always set.
inline constexpr std::uint64_t INITIAL_RFLAGS = 0x202;

/// @brief Whether @p addr is one user code may use: the lower half, without page zero.
[[nodiscard]] constexpr bool is_user_address(const memory::VirtualAddress addr) noexcept {
  return !addr.is_upper_half() && addr.value() >= memory::PAGE_SIZE;
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
