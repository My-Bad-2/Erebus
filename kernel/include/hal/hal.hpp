#pragma once

#include "patch.h"
#include "patcher.hpp"

#include <cstdint>

namespace kernel::hw {
namespace detail {
inline constexpr uint32_t RELAX_TSC_DELAY = 250;
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_waitpkg = {false};
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_moniterx = {false};
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_mwait = {false};
} // namespace detail

[[gnu::always_inline]] inline std::uint64_t read_tsc(std::uint32_t *cpu_id) noexcept {
  std::uint32_t cpu, low, high;
  asm volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(cpu) : /* No Input */ : "memory");

  if (cpu_id) [[likely]] {
    *cpu_id = cpu;
  }

  return (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
}

[[gnu::always_inline]] inline std::uint64_t read_tsc() noexcept { return read_tsc(nullptr); }

[[gnu::always_inline]] inline void pause() noexcept {
  asm volatile("pause" : /* No Output */ : /* No Input */ : "memory");
}

[[gnu::always_inline]] inline void tpause(const std::uint32_t state = 1,
                                          const std::uint64_t delay = detail::RELAX_TSC_DELAY) noexcept {
  std::uint64_t target_tsc = read_tsc() + delay;
  const std::uint32_t target_hi = static_cast<std::uint32_t>(target_tsc >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(target_tsc & 0xFFFFFFFF);

  asm volatile("tpause %%ecx" : : "c"(state), "a"(target_lo), "d"(target_hi) : "memory");
}

[[gnu::always_inline]] inline void cpu_relax(const std::uint32_t state = 1,
                                             const std::uint64_t delay = detail::RELAX_TSC_DELAY) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_waitpkg)) {
    tpause(state, delay);
  } else {
    pause();
  }
}

namespace irq {
[[gnu::always_inline]] inline void disable() noexcept { asm volatile("cli" ::: "memory"); }
[[gnu::always_inline]] inline void enable() noexcept { asm volatile("sti" ::: "memory"); }
[[gnu::always_inline]] inline std::uint64_t read_flags() noexcept {
  std::uint64_t rflags;
  asm volatile("pushfq\n\t"
               "popq %0"
               : "=r"(rflags)
               : /* No Input */
               : "memory");
  return rflags;
}

[[gnu::always_inline]] inline void write_flags(const std::uint64_t flags) noexcept {
  asm volatile("pushfq %0\n\t"
               "popfq"
               : /* No Output */
               : "r"(flags)
               : "cc", "memory");
}

[[gnu::always_inline]] inline bool is_enabled() noexcept { return (read_flags() & (1UL << 9)) != 0; }
} // namespace irq

[[gnu::always_inline]] inline void wait_for_interrupt() noexcept {
  while (true) {
    asm volatile("sti\n\t"
                 "hlt\n\t"
                 : /* No output */
                 : /* No Input*/
                 : "memory");
  }
}

[[gnu::always_inline, noreturn]] inline void dead_loop() noexcept {
  while (true) {
    asm volatile("cli\n\t"
                 "hlt\n\t"
                 : /* No output */
                 : /* No Input*/
                 : "memory");
  }
}

namespace detail {
[[gnu::always_inline]] inline void idle_generic(volatile std::uint32_t *runqueue_count) noexcept {
  while (*runqueue_count == 0) {
    wait_for_interrupt();
  }
}

[[gnu::always_inline]] inline void idle_mwait(volatile std::uint32_t *runqueue_count) noexcept {
  while (*runqueue_count == 0) {
    asm volatile("monitor"
                 :                                                                    /* No output */
                 : "a"(runqueue_count) /* Address */, "c"(0) /* Extensions */, "d"(0) /* Hints */
                 : "memory");

    if (*runqueue_count != 0) {
      break;
    }

    asm volatile("mwait" : /* No Output */ : "a"(0) /* C1 state hint */, "c"(1) /* Break on interrupts */ : "memory");
  }
}
} // namespace detail

[[gnu::always_inline]] inline void cpu_idle_loop(volatile std::uint32_t *runqueue_count) noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_mwait)) {
    detail::idle_mwait(runqueue_count);
  } else {
    detail::idle_generic(runqueue_count);
  }
}

[[gnu::always_inline]] inline void umonitor(const volatile void *addr) noexcept {
  asm volatile("umonitor %0" : /* No Output */ : "r"(addr) : "memory");
}

[[gnu::always_inline]] inline void umwait(const std::uint32_t state, const std::uint64_t tsc_deadline) noexcept {
  const std::uint32_t target_hi = static_cast<std::uint32_t>(tsc_deadline >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(tsc_deadline & 0xFFFFFFFF);

  asm volatile("umwait %2" : : "a"(target_lo), "d"(target_hi), "r"(state) : "cc", "memory");
}

[[gnu::always_inline]] inline void monitorx(const volatile void *addr, const std::uint32_t extensions,
                                            const std::uint32_t hints) noexcept {
  asm volatile("monitorx" : /* No Output */ : "a"(addr), "c"(extensions), "d"(hints) : "memory");
}

[[gnu::always_inline]] inline void mwaitx(const std::uint32_t extension, const std::uint32_t hints,
                                          const std::uint32_t timeout) noexcept {
  asm volatile("mwaitx" : /* No Output */ : "a"(hints), "c"(extension), "b"(timeout) : "memory");
}

void initialize() noexcept;
} // namespace kernel::hw