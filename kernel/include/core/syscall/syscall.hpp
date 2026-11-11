/// @file syscall.hpp
/// @brief Where a system call goes once it is in the kernel.
///
/// Getting in is the CPU's business: hal/syscall.hpp for SYSCALL without FRED, and the interrupt
/// entry (hal/interrupts) for FRED, which delivers it as an event. Both hand the registers to
/// handle() and leave by the way they came. The numbers, and which register is which, are in
/// abi.hpp.
///
/// A number names a family and a function in it, and the handlers are in one table, with a range of
/// it for each family: finding one is a lookup in each part, bounds-checked, the same for every
/// number. The checks are done in a way that does not let the CPU guess past them, as the number is
/// whatever user code put in RAX.
#pragma once

#include <cstdint>

#include "hal/interrupts/interrupts.hpp"

namespace kernel::core::syscall {
struct UserReturn {
  std::uint64_t rip;  ///< The instruction after the SYSCALL.
  std::uint64_t rsp;
  std::uint64_t rflags;
};

/// @brief Runs the system call @p regs ask for and leaves its results in them. @p from is where
///        the caller goes back to, for calls that need to know, such as a fork that returns twice.
/// @pre Called on the way in from ring 3, on the calling thread's kernel stack, with interrupts
///      off and nothing else that keeps it from sleeping.
/// @post Interrupts are off again.
void handle(hw::interrupts::RegisterContext& regs, const UserReturn& from) noexcept;
}  // namespace kernel::core::syscall
