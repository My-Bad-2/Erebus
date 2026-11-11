/// @file syscall.hpp
/// @brief How a SYSCALL from user code gets into the kernel, and back out.
///
/// Without FRED, SYSCALL jumps to the address in IA32_LSTAR, still on the user's stack and with the
/// user's GS. The entry here swaps GS, switches to the thread's kernel stack, and saves the
/// registers in the same order the interrupt entry does, under the frame an interrupt from ring 3
/// would have left. core::syscall::handle() gets the registers, and the scheduler gets its say
/// (sched::Scheduler::return_to_user()). The way out is SYSRET where it can be, which is almost
/// always, and IRET where it cannot.
///
/// With FRED, SYSCALL is an event like any other and arrives through the interrupt entry
/// (hal/interrupts), which hands it to the same handle(). SYSRET is not allowed then.
///
/// Either way IA32_STAR has to hold the kernel's selectors, as FRED takes ring 0's from there too.
#pragma once

namespace kernel::hw::syscall {
/// @brief Lets the calling CPU take system calls. Run by every CPU on itself, after
///        interrupts::initialize(), which decides whether there is FRED.
void initialize() noexcept;
}  // namespace kernel::hw::syscall
