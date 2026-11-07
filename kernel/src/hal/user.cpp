#include "hal/user.hpp"

#include "hal/hal.hpp"
#include "hal/smp/cpu_info.hpp"
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

void clear_ac() noexcept { clear_ac_flag(); }

std::uint64_t read_gs_base() noexcept { return read::msr(IA32_KERNEL_GS_BASE); }

void write_gs_base(const std::uint64_t base) noexcept { write::msr(IA32_KERNEL_GS_BASE, base); }

bool can_change_bases() noexcept { return fsgsbase_enabled(); }

std::uint32_t read_pkru() noexcept { return read_pkru_register(); }
}  // namespace kernel::hw::user
