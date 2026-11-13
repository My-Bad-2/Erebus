// The system call instruction itself, as misc/syscalls.md describes it: numbers, and which
// registers come back as they went in.
#include <array>
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "sys/abi.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
// A system call with a number the typed wrappers cannot express.
[[nodiscard]] std::int64_t raw_syscall(const std::uint64_t number) noexcept {
  std::int64_t rax = static_cast<std::int64_t>(number);
  asm volatile("syscall" : "+a"(rax) : : "rcx", "r11", "memory");
  return rax;
}

constexpr std::int64_t NOT_IMPLEMENTED = sys::encode_error(sys::Error::NotImplemented);
}  // namespace

KTEST(Syscall, a_number_nothing_answers_to_is_not_implemented) {
  constexpr std::uint64_t DEBUG_LOG = std::to_underlying(sys::Syscall::DebugLog);
  constexpr std::uint64_t NUMBERS[] = {
      sys::make_number(sys::Family::Ipc, 0xFF),                // past the end of a family
      sys::make_number(sys::Family::Debug, 0x01),              // ditto, the last family
      std::uint64_t{sys::FAMILY_COUNT} << sys::FUNCTION_BITS,  // the first family there is not
      0xF00,                                                   // the last one there could be
      DEBUG_LOG | (std::uint64_t{1} << 12),  // a real call with bits above the family
      DEBUG_LOG | (std::uint64_t{1} << 63),
  };

  for (const std::uint64_t number : NUMBERS) {
    KEXPECT_EQ(raw_syscall(number), NOT_IMPLEMENTED);
  }
}

KTEST(Syscall, reserved_calls_say_so) {
  KEXPECT_ERROR(sys::thread::set_priority(), sys::Error::NotImplemented);
}

// RAX has the result, and RCX and R11 are SYSCALL's own. Every other register comes back as it
// went in, whichever way the kernel returns.
KTEST(Syscall, keeps_every_register_but_rax_rcx_and_r11) {
  constexpr std::uint64_t PATTERN = 0x5EED'0000'0000'0000;
  register std::uint64_t rbx asm("rbx") = PATTERN | 1;
  register std::uint64_t rdx asm("rdx") = PATTERN | 2;
  register std::uint64_t rsi asm("rsi") = PATTERN | 3;
  register std::uint64_t rdi asm("rdi") = PATTERN | 4;
  register std::uint64_t r8 asm("r8") = PATTERN | 5;
  register std::uint64_t r9 asm("r9") = PATTERN | 6;
  register std::uint64_t r10 asm("r10") = PATTERN | 7;
  register std::uint64_t r12 asm("r12") = PATTERN | 8;
  register std::uint64_t r13 asm("r13") = PATTERN | 9;
  register std::uint64_t r14 asm("r14") = PATTERN | 10;
  register std::uint64_t r15 asm("r15") = PATTERN | 11;
  std::uint64_t rax = std::to_underlying(sys::Syscall::ThreadId);

  asm volatile("syscall"
               : "+a"(rax), "+r"(rbx), "+r"(rdx), "+r"(rsi), "+r"(rdi), "+r"(r8), "+r"(r9),
                 "+r"(r10), "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
               :
               : "rcx", "r11", "memory");

  const std::array<std::uint64_t, 11> after{rbx, rdx, rsi, rdi, r8, r9, r10, r12, r13, r14, r15};
  for (std::size_t i = 0; i < after.size(); ++i) {
    KEXPECT_EQ(after[i], PATTERN | (i + 1));
  }

  KEXPECT_EQ(rax, sys::thread::id());
}

// RFLAGS comes back too, through R11. The direction flag says so without a push, which would
// write over the red zone: a string instruction goes the other way when it is set.
KTEST(Syscall, keeps_the_direction_flag) {
  static constexpr char TEXT[] = "ab";
  const char* cursor = &TEXT[1];
  std::uint64_t rax = std::to_underlying(sys::Syscall::ThreadId);

  asm volatile(
      "std\n\t"
      "syscall\n\t"
      "lodsb\n\t"
      "cld"
      : "+a"(rax), "+S"(cursor)
      :
      : "rcx", "r11", "memory");

  KEXPECT(cursor == &TEXT[0]);
}

// What the kernel saves for a thread that blocks is restored when it runs again, whoever ran on
// its CPU meanwhile. Every participant puts its own values in, so that one getting another's
// would show.
KTEST_ALL_CPUS(Syscall, keeps_the_vector_registers_across_a_sleep, cpu) {
  using Vector = long long __attribute__((vector_size(16)));
  const auto mark = [cpu](const long long lane) {
    return static_cast<long long>(0xF00D'0000'0000 + (std::uint64_t{cpu} << 16)) + lane;
  };

  for (int round = 0; round < 20; ++round) {
    register Vector x0 asm("xmm0") = {mark(0), mark(1)};
    register Vector x7 asm("xmm7") = {mark(2), mark(3)};
    register Vector x8 asm("xmm8") = {mark(4), mark(5)};
    register Vector x15 asm("xmm15") = {mark(6), mark(7)};

    // TimeSleep for 200 microseconds: RDI the time, RSI the slack, RDX the flags.
    std::uint64_t rax = std::to_underlying(sys::Syscall::TimeSleep);
    register std::uint64_t rdi asm("rdi") = 200'000;
    register std::uint64_t rsi asm("rsi") = 0;
    register std::uint64_t rdx asm("rdx") = 0;
    asm volatile("syscall"
                 : "+a"(rax), "+x"(x0), "+x"(x7), "+x"(x8), "+x"(x15)
                 : "r"(rdi), "r"(rsi), "r"(rdx)
                 : "rcx", "r11", "memory");

    KEXPECT_EQ(rax, 0u);
    KEXPECT(x0[0] == mark(0) && x0[1] == mark(1));
    KEXPECT(x7[0] == mark(2) && x7[1] == mark(3));
    KEXPECT(x8[0] == mark(4) && x8[1] == mark(5));
    KEXPECT(x15[0] == mark(6) && x15[1] == mark(7));
  }
}
}  // namespace user::testing
