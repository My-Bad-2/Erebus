#include "hal/syscall.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/syscall/abi.hpp"
#include "core/syscall/syscall.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "hal/msr.hpp"
#include "hal/smp/cpu_info.hpp"
#include "hal/smp/gdt.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/user.hpp"
#include "interrupts/register_asm.hpp"
#include "sched/scheduler.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::syscall {
namespace {
// What the entry leaves on the kernel stack: the registers, and above them the frame an interrupt
// from ring 3 would have pushed, so that the way back can be IRET as well as SYSRET.
struct Frame {
  interrupts::RegisterContext regs;
  std::uint64_t rip;
  std::uint64_t cs;
  std::uint64_t rflags;
  std::uint64_t rsp;
  std::uint64_t ss;
};

static_assert(offsetof(Frame, rip) == sizeof(interrupts::RegisterContext));
static_assert(sizeof(Frame) % 16 == 0, "the C++ half is called with the stack the ABI wants");

constexpr std::size_t KERNEL_STACK_SLOT = offsetof(PerCpu, syscall_stack);
constexpr std::size_t USER_STACK_SLOT = offsetof(PerCpu, syscall_scratch);

// SYSCALL loads CS from the base in IA32_STAR and SS from the entry after it. SYSRET loads SS from
// the entry after its base and CS from the one after that, which is why user data comes before
// user code in the GDT.
//
// SYSRET's base carries RPL 3. Intel CPUs force it to 3 in both selectors anyway, but AMD CPUs take
// SS's from the base as it is, and user code would run with an SS of RPL 0. Nothing minds in
// 64-bit mode until the first interrupt from ring 3 pushes that SS, and the IRET back refuses it
// with a #GP, in ring 0, after SWAPGS has already put the user's GS back.
constexpr std::uint16_t SYSCALL_BASE = std::to_underlying(gdt::Selector::KernelCode);
constexpr std::uint16_t SYSRET_BASE =
    std::to_underlying(gdt::with_rpl(gdt::Selector::KernelData, gdt::PrivilegeLevel::Ring3));
static_assert(SYSCALL_BASE + 8 == std::to_underlying(gdt::Selector::KernelData));
static_assert(SYSRET_BASE + 8 == user::DATA_SELECTOR, "SS after SYSRET, RPL 3 and all");
static_assert(SYSRET_BASE + 16 == user::CODE_SELECTOR, "CS after SYSRET, RPL 3 and all");

// What SYSCALL clears, which is whatever would change how kernel code runs: TF would single-step
// it, IF would let an interrupt in before there is a kernel stack, DF would run string
// instructions backwards, AC would turn SMAP off, and NT would make the IRET out fault. RF and
// IOPL are not the kernel's to inherit either. The arithmetic flags do not matter to it.
constexpr RFlags CLEARED_ON_ENTRY = RFlags{}
                                        .with_trap_flag()
                                        .with_interrupt_flag()
                                        .with_direction_flag()
                                        .with_io_privilege_level(3)
                                        .with_nested_task()
                                        .with_resume_flag()
                                        .with_alignment_check();

// Where SYSCALL left is not canonical. Nothing in the kernel sends a thread there, and nothing can
// be mapped where SYSCALL could leave one, so this is a bug. Neither way back could take it: both
// fault in ring 0 on Intel CPUs, with the user's GS loaded. The thread gets the #GP it would have
// had on AMD, where the fault comes after the return, and does not come back from it.
[[noreturn]] void refuse_return(Frame& frame) noexcept {
  const interrupts::Event event{
      .category = interrupts::EventClass::Exception,
      .vector = std::to_underlying(interrupts::InterruptVector::GeneralProtectionFault),
      .cpl = interrupts::EventCpl::Ring3,
      .error_code = 0,
      .payload = 0,
      .rip = frame.rip,
      .rsp = frame.rsp,
      .rflags = frame.rflags,
      .gprs = &frame.regs,
  };

  interrupts::InterruptManager::route_event(event);
  sched::Scheduler::exit_current();
}

// The C++ half of entry(), with interrupts off on the way in and out.
// @return Whether the way back can be SYSRET.
[[gnu::used]] bool from_user(Frame* const frame) noexcept {
  core::syscall::handle(frame->regs,
                        {.rip = frame->rip, .rsp = frame->rsp, .rflags = frame->rflags});
  sched::Scheduler::return_to_user();

  if (frame->rip >= user::lower_half_end()) [[unlikely]] {
    refuse_return(*frame);
  }

  // SYSRET cannot restore RF, and with TF it traps right after it rather than one instruction
  // later. Neither is ever set by the time SYSCALL saves RFLAGS, short of a debugger.
  const RFlags flags{frame->rflags};
  return !flags.get_trap_flag() && !flags.get_resume_flag();
}

// SYSCALL from 64-bit user code. Interrupts are off, as IA32_FMASK says, but nothing else is the
// kernel's yet: not GS, and not the stack. An NMI in that window takes care of GS by itself (see
// legacy_nmi_common()), and runs on a stack of its own.
//
// The registers are cleared once they are saved, so that no value user code chose is around for
// the kernel to speculate with. The handlers read the arguments from the frame.
[[gnu::naked]] void entry() noexcept {
  asm volatile(
      "swapgs\n\t"
      "movq %%rsp, %%gs:%c[user_stack]\n\t"
      "movq %%gs:%c[kernel_stack], %%rsp\n\t"

      "pushq %[user_ss]\n\t"
      "pushq %%gs:%c[user_stack]\n\t"
      "pushq %%r11\n\t"  // RFLAGS, as SYSCALL left it
      "pushq %[user_cs]\n\t"
      "pushq %%rcx\n\t"  // RIP, as SYSCALL left it

      ASM_PUSH_ALL_REGISTERS

      "xorl %%eax, %%eax\n\t"
      "xorl %%ebx, %%ebx\n\t"
      "xorl %%ecx, %%ecx\n\t"
      "xorl %%edx, %%edx\n\t"
      "xorl %%esi, %%esi\n\t"
      "xorl %%ebp, %%ebp\n\t"
      "xorl %%r8d, %%r8d\n\t"
      "xorl %%r9d, %%r9d\n\t"
      "xorl %%r10d, %%r10d\n\t"
      "xorl %%r11d, %%r11d\n\t"
      "xorl %%r12d, %%r12d\n\t"
      "xorl %%r13d, %%r13d\n\t"
      "xorl %%r14d, %%r14d\n\t"
      "xorl %%r15d, %%r15d\n\t"

      "movq %%rsp, %%rdi\n\t"
      "call %P[from_user]\n\t"
      "testb %%al, %%al\n\t"
      "jz 1f\n\t"

      // SYSRET takes RIP from RCX and RFLAGS from R11, which is why a system call loses both.
      ASM_POP_ALL_REGISTERS
      "movq (%%rsp), %%rcx\n\t"
      "movq 16(%%rsp), %%r11\n\t"
      "movq 24(%%rsp), %%rsp\n\t"
      "swapgs\n\t"
      "sysretq\n\t"

      "1:\n\t" ASM_POP_ALL_REGISTERS
      "swapgs\n\t"
      "iretq\n\t"
      : /* No Output */
      : [user_stack] "i"(USER_STACK_SLOT), [kernel_stack] "i"(KERNEL_STACK_SLOT),
        [user_ss] "i"(user::DATA_SELECTOR), [user_cs] "i"(user::CODE_SELECTOR),
        [from_user] "s"(&from_user)
      : "memory");
}

// SYSCALL from compatibility mode, which only AMD CPUs send here. There is no 32-bit user code,
// as the GDT has no 32-bit code segment for it, so this is for completeness: straight back, with
// the error a call nobody answers gets, and without touching the stack or GS.
[[gnu::naked]] void compat_entry() noexcept {
  asm volatile(
      "movl %[error], %%eax\n\t"
      "sysretl\n\t"
      : /* No Output */
      : [error] "i"(static_cast<std::int32_t>(core::syscall::encode_error(Error::NotImplemented)))
      : "memory");
}
}  // namespace

void initialize() noexcept {
  const CpuInfo* const cpu = profile_manager.get_current();
  if (!cpu->has<Feature::SYSCALL>()) [[unlikely]] {
    utils::logger::fatal("Syscall: this CPU has no SYSCALL instruction\n");
  }

  // Without it SYSCALL raises #UD, FRED or not.
  write::efer(read::efer().with_sce());

  // FRED takes the selectors of ring 0 from here as well.
  write::msr(MSR_IA32_STAR,
             MsrSyscallStar{}.with_syscall_base(SYSCALL_BASE).with_sysret_base(SYSRET_BASE).raw());

  // The rest is for SYSCALL and SYSENTER to go somewhere. FRED delivers both as events, and SYSRET
  // and SYSEXIT raise #UD under it.
  if (cpu->has<Feature::FRED>()) {
    return;
  }

  write::msr(MSR_IA32_LSTAR, MsrSyscallTarget{memory::VirtualAddress{&entry}}.raw());
  write::msr(MSR_IA32_FMASK, MsrSyscallFlagMask{}
                                 .with_flag_mask(static_cast<std::uint32_t>(CLEARED_ON_ENTRY.raw()))
                                 .raw());

  // Intel CPUs raise #UD for SYSCALL from compatibility mode and never look at IA32_CSTAR, and in
  // a TDX guest writing it at all is a #VE.
  if (cpu->vendor() != Vendor::Intel) {
    write::msr(MSR_IA32_CSTAR, MsrSyscallTarget{memory::VirtualAddress{&compat_entry}}.raw());
  }

  // SYSENTER has no business here either, and with a null selector it raises #GP rather than go
  // wherever the firmware left these.
  if (cpu->has<Feature::SEP>()) {
    write::msr(MSR_IA32_SYSENTER_CS, 0);
    write::msr(MSR_IA32_SYSENTER_ESP, 0);
    write::msr(MSR_IA32_SYSENTER_EIP, 0);
  }
}
}  // namespace kernel::hw::syscall
