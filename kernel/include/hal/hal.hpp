#pragma once

#include "patch.h"
#include "patcher.hpp"

#include <cstdint>
#include <x86intrin.h>

namespace kernel::hw {
[[gnu::always_inline]] inline std::uint64_t read_tsc(std::uint32_t *cpu_id) noexcept {
  std::uint32_t cpu;
  const std::uint64_t val = __rdtscp(&cpu);
  if (cpu_id) [[likely]] {
    *cpu_id = cpu;
  }

  return val;
}

[[gnu::always_inline]] inline std::uint64_t read_tsc() noexcept {
  std::uint32_t junk;
  return __rdtscp(&junk);
}

namespace detail {
inline constexpr uint32_t RELAX_TSC_DELAY = 250;
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_tpause = {false};

[[gnu::always_inline]] inline void relax_generic() noexcept { _mm_pause(); }

[[gnu::always_inline]] inline void relax_tpause() noexcept {
  std::uint64_t target_tsc = read_tsc() + RELAX_TSC_DELAY;
  const std::uint32_t target_hi = static_cast<std::uint32_t>(target_tsc >> 32);
  const std::uint32_t target_lo = static_cast<std::uint32_t>(target_tsc & 0xFFFFFFFF);

  constexpr std::uint32_t state = 1;

  asm volatile("tpause %%ecx" : : "c"(state), "a"(target_lo), "d"(target_hi) : "memory");
}
} // namespace detail

[[gnu::always_inline]] inline void cpu_relax() noexcept {
  if (EREBUS_STATIC_BRANCH_UNLIKELY(detail::use_tpause)) {
    detail::relax_tpause();
  } else {
    detail::relax_generic();
  }
}

namespace irq {
[[gnu::always_inline]] inline void disable() noexcept { asm volatile("cli" ::: "memory"); }
[[gnu::always_inline]] inline void enable() noexcept { asm volatile("sti" ::: "memory"); }
[[gnu::always_inline]] inline std::uint64_t read_flags() noexcept { return __readeflags(); }
[[gnu::always_inline]] inline void write_flags(const std::uint64_t flags) noexcept { __writeeflags(flags); }
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
[[gnu::visibility("hidden")]] static patcher::StaticKeyDef use_mwait = {false};

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

void initialize() noexcept;
} // namespace kernel::hw