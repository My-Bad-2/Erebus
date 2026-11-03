#include "hal/hal.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::utils {
namespace {
constexpr std::uint32_t BASE_PAUSE_LOOPS = 64;
constexpr std::uint32_t BASE_WAIT_TSC = 2000;

// UMWAIT/TPAUSE state: C0.1 wakes up faster than C0.2, and a lock waiter cares more about how
// soon it notices the lock than about the power it saves.
constexpr std::uint32_t LIGHT_SLEEP = 1;

// Sleeps until *addr changes from `curr` or BASE_WAIT_TSC have passed. Arming the monitor
// before checking the value is what makes this race-free: a store that lands in between still
// ends the wait. The time limit bounds the damage if the value changes some other way.
void wait_umonitor(std::atomic<std::uint32_t>* addr, const std::uint32_t curr) noexcept {
  hw::umonitor(addr);
  if (addr->load(std::memory_order_relaxed) == curr) {
    hw::umwait(LIGHT_SLEEP, hw::read_tsc() + BASE_WAIT_TSC);
  }
}

void wait_monitorx(std::atomic<std::uint32_t>* addr, const std::uint32_t curr) noexcept {
  // ECX bit 1 turns on the MWAITX timer, with the limit in EBX.
  constexpr std::uint32_t MWAITX_TIMER_ENABLE = 1u << 1;

  hw::monitorx(addr, 0, 0);
  if (addr->load(std::memory_order_relaxed) == curr) {
    hw::mwaitx(MWAITX_TIMER_ENABLE, 0, BASE_WAIT_TSC);
  }
}

void wait_pause(std::atomic<std::uint32_t>*, std::uint32_t) noexcept { hw::pause(); }

// Backoff proportional to how many tickets are ahead of us.
void wait_time_tpause(const std::uint32_t distance) noexcept {
  hw::tpause(LIGHT_SLEEP, static_cast<std::uint64_t>(distance) * BASE_WAIT_TSC);
}

void wait_time_pause(const std::uint32_t distance) noexcept {
  for (std::uint32_t delay = distance * BASE_PAUSE_LOOPS; delay > 0; --delay) {
    hw::pause();
  }
}
}  // namespace

namespace detail {
DEFINE_STATIC_CALL(wait_monitor, wait_pause);
REGISTER_STATIC_CALL_ALT_PRIO(wait_monitor, wait_umonitor, 200, hw::Feature::WAITPKG);
REGISTER_STATIC_CALL_ALT_PRIO(wait_monitor, wait_monitorx, 100, hw::Feature::MONITORX);

DEFINE_STATIC_CALL(wait_time, wait_time_pause);
REGISTER_STATIC_CALL_ALT(wait_time, wait_time_tpause, hw::Feature::WAITPKG);
}  // namespace detail
}  // namespace kernel::utils
