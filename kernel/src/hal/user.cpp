#include "hal/user.hpp"

#include <cstddef>

#include "hal/hal.hpp"
#include "hal/smp/cpu_info.hpp"
#include "interrupts/register_asm.hpp"
#include "patch/static_call.hpp"

namespace kernel::hw::user {
namespace details {
// Both take the entry point in RDI, the stack in RSI and the argument in RDX, and clear every
// register they are not passing on, so nothing of the kernel's goes along.
#define USER_CLEAR_REGISTERS \
  "xorl %%eax, %%eax\n\t"    \
  "xorl %%ebx, %%ebx\n\t"    \
  "xorl %%ecx, %%ecx\n\t"    \
  "xorl %%edx, %%edx\n\t"    \
  "xorl %%esi, %%esi\n\t"    \
  "xorl %%ebp, %%ebp\n\t"    \
  "xorl %%r8d, %%r8d\n\t"    \
  "xorl %%r9d, %%r9d\n\t"    \
  "xorl %%r10d, %%r10d\n\t"  \
  "xorl %%r11d, %%r11d\n\t"  \
  "xorl %%r12d, %%r12d\n\t"  \
  "xorl %%r13d, %%r13d\n\t"  \
  "xorl %%r14d, %%r14d\n\t"  \
  "xorl %%r15d, %%r15d\n\t"

[[gnu::naked]] void enter_iret(std::uint64_t, std::uint64_t, std::uint64_t) noexcept {
  asm volatile(
      "pushq %[ss]\n\t"
      "pushq %%rsi\n\t"
      "pushq %[flags]\n\t"
      "pushq %[cs]\n\t"
      "pushq %%rdi\n\t"
      "movq %%rdx, %%rdi\n\t"

      USER_CLEAR_REGISTERS

      "swapgs\n\t"
      "iretq\n\t"
      : /* No Output */
      : [ss] "i"(DATA_SELECTOR), [cs] "i"(CODE_SELECTOR), [flags] "i"(INITIAL_RFLAGS));
}

// The frame FRED event delivery would have pushed, error code included, which ERETU skips. It
// swaps GS the way SWAPGS would.
[[gnu::naked]] void enter_eretu(std::uint64_t, std::uint64_t, std::uint64_t) noexcept {
  asm volatile(
      "andq $-64, %%rsp\n\t"
      "pushq $0\n\t"  // Reserved
      "pushq $0\n\t"  // Event data
      "pushq %[ss]\n\t"
      "pushq %%rsi\n\t"
      "pushq %[flags]\n\t"
      "pushq %[cs]\n\t"
      "pushq %%rdi\n\t"
      "pushq $0\n\t"  // Error code
      "movq %%rdx, %%rdi\n\t"

      USER_CLEAR_REGISTERS

      "eretu\n\t"
      : /* No Output */
      : [ss] "i"(DATA_SELECTOR), [cs] "i"(CODE_SELECTOR), [flags] "i"(INITIAL_RFLAGS));
}

#undef USER_CLEAR_REGISTERS

// Pushes the registers of the ResumeFrame RDI points at, in the order ASM_POP_ALL_REGISTERS pops
// them, so that popping them takes every one, RDI included, at once.
#define USER_PUSH_FRAME_REGISTERS \
  "pushq 112(%%rdi)\n\t"          \
  "pushq 104(%%rdi)\n\t"          \
  "pushq 96(%%rdi)\n\t"           \
  "pushq 88(%%rdi)\n\t"           \
  "pushq 80(%%rdi)\n\t"           \
  "pushq 72(%%rdi)\n\t"           \
  "pushq 64(%%rdi)\n\t"           \
  "pushq 56(%%rdi)\n\t"           \
  "pushq 48(%%rdi)\n\t"           \
  "pushq 40(%%rdi)\n\t"           \
  "pushq 32(%%rdi)\n\t"           \
  "pushq 24(%%rdi)\n\t"           \
  "pushq 16(%%rdi)\n\t"           \
  "pushq 8(%%rdi)\n\t"            \
  "pushq 0(%%rdi)\n\t"

using Registers = interrupts::RegisterContext;
static_assert(offsetof(Registers, r15) == 0 && offsetof(Registers, r14) == 8 &&
                  offsetof(Registers, r13) == 16 && offsetof(Registers, r12) == 24 &&
                  offsetof(Registers, r11) == 32 && offsetof(Registers, r10) == 40 &&
                  offsetof(Registers, r9) == 48 && offsetof(Registers, r8) == 56 &&
                  offsetof(Registers, rbp) == 64 && offsetof(Registers, rdi) == 72 &&
                  offsetof(Registers, rsi) == 80 && offsetof(Registers, rdx) == 88 &&
                  offsetof(Registers, rcx) == 96 && offsetof(Registers, rbx) == 104 &&
                  offsetof(Registers, rax) == 112,
              "USER_PUSH_FRAME_REGISTERS knows these offsets");

// Both take the frame in RDI, which stays put while the stack below it is built: the frame a return
// to user mode wants, then the registers, which come off last.
[[gnu::naked]] void resume_iret(const ResumeFrame*) noexcept {
  asm volatile(
      "pushq %[ss]\n\t"
      "pushq %c[rsp](%%rdi)\n\t"
      "pushq %c[rflags](%%rdi)\n\t"
      "pushq %[cs]\n\t"
      "pushq %c[rip](%%rdi)\n\t"

      USER_PUSH_FRAME_REGISTERS  // and off again, into the registers
      ASM_POP_ALL_REGISTERS

      "swapgs\n\t"
      "iretq\n\t"
      : /* No Output */
      : [ss] "i"(DATA_SELECTOR), [cs] "i"(CODE_SELECTOR), [rip] "i"(offsetof(ResumeFrame, rip)),
        [rsp] "i"(offsetof(ResumeFrame, rsp)), [rflags] "i"(offsetof(ResumeFrame, rflags)));
}

[[gnu::naked]] void resume_eretu(const ResumeFrame*) noexcept {
  asm volatile(
      "andq $-64, %%rsp\n\t"
      "pushq $0\n\t"  // Reserved
      "pushq $0\n\t"  // Event data
      "pushq %[ss]\n\t"
      "pushq %c[rsp](%%rdi)\n\t"
      "pushq %c[rflags](%%rdi)\n\t"
      "pushq %[cs]\n\t"
      "pushq %c[rip](%%rdi)\n\t"
      "pushq $0\n\t"  // Error code

      USER_PUSH_FRAME_REGISTERS  // and off again, into the registers
      ASM_POP_ALL_REGISTERS

      "eretu\n\t"
      : /* No Output */
      : [ss] "i"(DATA_SELECTOR), [cs] "i"(CODE_SELECTOR), [rip] "i"(offsetof(ResumeFrame, rip)),
        [rsp] "i"(offsetof(ResumeFrame, rsp)), [rflags] "i"(offsetof(ResumeFrame, rflags)));
}

#undef USER_PUSH_FRAME_REGISTERS

void clac() noexcept { asm volatile("clac" ::: "memory"); }

std::uint32_t no_pkru() noexcept { return 0; }

std::uint32_t rdpkru() noexcept {
  std::uint32_t pkru;
  std::uint32_t edx;
  asm volatile("rdpkru" : "=a"(pkru), "=d"(edx) : "c"(0));
  return pkru;
}
}  // namespace details

namespace {
constexpr std::uint32_t IA32_KERNEL_GS_BASE = 0xC0000102;
}  // namespace

DECLARE_STATIC_CALL(void, enter_user_mode, std::uint64_t, std::uint64_t, std::uint64_t);
DEFINE_STATIC_CALL(enter_user_mode, details::enter_iret);
REGISTER_STATIC_CALL_ALT(enter_user_mode, details::enter_eretu, Feature::FRED);

DECLARE_STATIC_CALL(void, resume_user_mode, const ResumeFrame*);
DEFINE_STATIC_CALL(resume_user_mode, details::resume_iret);
REGISTER_STATIC_CALL_ALT(resume_user_mode, details::resume_eretu, Feature::FRED);

DECLARE_STATIC_CALL(void, clear_ac_flag);
DEFINE_STATIC_CALL_NOP(clear_ac_flag);
REGISTER_STATIC_CALL_ALT(clear_ac_flag, details::clac, Feature::SMAP);

DECLARE_STATIC_CALL(std::uint32_t, read_pkru_register);
DEFINE_STATIC_CALL(read_pkru_register, details::no_pkru);
REGISTER_STATIC_CALL_ALT(read_pkru_register, details::rdpkru, Feature::PKU);

void enter(const memory::VirtualAddress pc, const memory::VirtualAddress sp,
           const std::uint64_t arg) noexcept {
  enter_user_mode(pc.value(), sp.value(), arg);
  std::unreachable();
}

void resume(const ResumeFrame& frame) noexcept {
  // The frame lives on this stack, above where the return frame goes, so it is copied first:
  // sanitised, and out of the way of whoever passed it.
  ResumeFrame copy = frame;
  copy.rflags = user_rflags(frame.rflags);
  resume_user_mode(&copy);
  std::unreachable();
}

void clear_ac() noexcept { clear_ac_flag(); }

std::uint64_t read_gs_base() noexcept { return read::msr(IA32_KERNEL_GS_BASE); }

void write_gs_base(const std::uint64_t base) noexcept { write::msr(IA32_KERNEL_GS_BASE, base); }

bool can_change_bases() noexcept { return fsgsbase_enabled(); }

std::uint32_t read_pkru() noexcept { return read_pkru_register(); }
}  // namespace kernel::hw::user
