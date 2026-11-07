// What the tests of everything that blocks have in common: real threads, and ways to wait for them
// that fail a test instead of hanging the run.
//
// Test bodies run on the boot thread of the BSP, which is its idle thread: it only gets the CPU
// when nothing else wants it. So a test that has a spinning thread on the BSP does not come back
// until that thread is done, and threads here have to end by themselves. Tests that need a thread
// to stay around pin it to another CPU, or have it sleep between looks.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstdint>
#include <string_view>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/smp.hpp"
#include "sched/scheduler.hpp"
#include "sched/sleep.hpp"
#include "sched/thread.hpp"

namespace kernel::testing::support {
inline constexpr klib::Duration PATIENCE = klib::Duration::seconds(30);

/// Long enough for a thread to have got somewhere, if it was going to.
inline constexpr klib::Duration A_MOMENT = klib::Duration::milliseconds(5);

/// Gives the CPU to whatever wants it until @p done says so.
template <typename Pred>
[[nodiscard]] bool wait_for(Pred&& done, const klib::Duration timeout = PATIENCE) noexcept {
  const klib::TimePoint deadline = drivers::SystemClock::now() + timeout;

  while (!done()) {
    if (drivers::SystemClock::now() > deadline) {
      return false;
    }

    sched::Scheduler::yield();
    hw::pause();
  }

  return true;
}

/// Waits for a flag from code that must not get in anyone's way while it does: a thread holding on
/// to something, say, and it may be on the BSP, so spinning is out.
inline void sleep_until_set(const std::atomic<bool>& flag) noexcept {
  while (!flag.load(std::memory_order_acquire)) {
    (void)sched::sleep_for(klib::Duration::microseconds(100));
  }
}

inline void spin_for(const klib::Duration time) noexcept {
  const klib::TimePoint end = drivers::SystemClock::now() + time;
  while (drivers::SystemClock::now() < end) {
    hw::pause();
  }
}

/// The last CPU: an AP if there is one, as the BSP is busy running the test.
[[nodiscard]] inline std::uint32_t last_cpu() noexcept { return hw::smp::online_cpus() - 1; }

[[nodiscard]] inline sched::ThreadCreateOptions pinned_to(const std::uint32_t cpu) noexcept {
  return {.preferred_cpu = cpu, .pinned = true};
}

/// Options for a thread of @p priority. @p pin_to picks a CPU to pin it to, if any.
[[nodiscard]] inline sched::ThreadCreateOptions of_class(
    const sched::PriorityClass priority,
    const std::uint32_t pin_to = sched::NO_PREFERENCE) noexcept {
  return {.priority = priority, .preferred_cpu = pin_to, .pinned = pin_to != sched::NO_PREFERENCE};
}

/// Starts a thread. @return It, or null, and it must not be touched once it may have ended.
template <typename Payload>
sched::Thread* start(const std::string_view name, void (*entry)(Payload*) noexcept,
                     Payload* payload, const sched::ThreadCreateOptions& opts = {}) noexcept {
  const auto created = sched::Thread::spawn_kernel(name, entry, payload, opts);
  if (!created || !sched::Scheduler::enqueue(*created)) {
    return nullptr;
  }

  return *created;
}

/// A cheap source of numbers for tests that shuffle things around. Same seed, same run.
class Rng {
  std::uint64_t m_state;

 public:
  explicit constexpr Rng(const std::uint64_t seed) noexcept : m_state(seed * 2 + 1) {}

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 7;
    m_state ^= m_state << 17;
    return m_state;
  }

  [[nodiscard]] constexpr std::uint64_t below(const std::uint64_t limit) noexcept {
    return next() % limit;
  }
};
}  // namespace kernel::testing::support
