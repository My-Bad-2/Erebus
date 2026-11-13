// The Arch and Debug families from user code.
#include <array>
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
// Neither is an address user code may use: the first is in the kernel's half, the second is not
// canonical.
constexpr std::uintptr_t KERNEL_HALF = 0xFFFF'8000'0000'0000;
constexpr std::uintptr_t NOT_CANONICAL = 0x0000'8000'0000'0000;

[[nodiscard]] bool cpu_has_amx() noexcept {
  std::uint32_t eax = 7;
  std::uint32_t ebx = 0;
  std::uint32_t ecx = 0;
  std::uint32_t edx = 0;
  asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  return (edx & (1u << 24)) != 0;  // AMX-TILE
}
}  // namespace

// Each participant keeps its own GS base, however often the threads change places on the CPUs.
KTEST_ALL_CPUS(Arch, each_thread_has_a_gs_base_of_its_own, cpu) {
  const auto mine = reinterpret_cast<void*>(0x4000'0000 + (std::uintptr_t{cpu} << 20));
  KREQUIRE_OK(sys::arch::set_gs_base(mine));

  for (int round = 0; round < 50; ++round) {
    sys::thread::yield();
    const auto now = sys::arch::get_gs_base();
    KREQUIRE_OK(now);
    KREQUIRE_EQ(*now, mine);
  }

  KEXPECT_OK(sys::arch::set_gs_base(nullptr));
  KEXPECT_EQ(sys::arch::get_gs_base().value_or(mine), nullptr);
}

KTEST(Arch, bases_outside_user_space_are_refused) {
  for (const std::uintptr_t base : {KERNEL_HALF, NOT_CANONICAL}) {
    KEXPECT_ERROR(sys::arch::set_gs_base(reinterpret_cast<void*>(base)),
                  sys::Error::InvalidArgument);
    KEXPECT_ERROR(sys::arch::set_fs_base(reinterpret_cast<void*>(base)),
                  sys::Error::InvalidArgument);
  }

  // Refused, so FS still points where the runtime put it.
  void* self = nullptr;
  asm volatile("movq %%fs:0, %0" : "=r"(self));
  KEXPECT_EQ(sys::arch::get_fs_base().value_or(nullptr), self);
}

KTEST(Arch, amx_is_only_there_on_a_cpu_that_has_it) {
  const auto enabled = sys::arch::enable_amx();
  if (!cpu_has_amx()) {
    KEXPECT_ERROR(enabled, sys::Error::NotImplemented);
  } else if (!enabled) {
    // The CPU has it, but the kernel may not have turned it on: say so, rather than pass.
    KTEST_SKIP("the CPU has AMX, but the kernel does not offer it");
  }
}

KTEST(Debug, log_takes_a_line_from_user_memory) {
  KEXPECT_OK(sys::debug::log("utest: a line from user space, as it is"));

  // Longer than a line may be: cut short, not refused.
  static std::array<char, 2 * sys::MAX_LOG_BYTES> long_line{};
  long_line.fill('-');
  KEXPECT_OK(sys::debug::log({long_line.data(), long_line.size()}));
}

KTEST(Debug, log_refuses_text_it_cannot_read) {
  KEXPECT_ERROR(sys::debug::log({reinterpret_cast<const char*>(KERNEL_HALF), 8}),
                sys::Error::InvalidArgument);
  // User space, but nothing is mapped there.
  KEXPECT_ERROR(sys::debug::log({reinterpret_cast<const char*>(0x2000), 8}), sys::Error::NotMapped);
}
}  // namespace user::testing
