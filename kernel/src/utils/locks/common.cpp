#include "hal/hal.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::utils {
namespace {
constexpr std::uint32_t BASE_PAUSE_LOOPS = 64;
constexpr std::uint32_t BASE_WAIT_TSC = 2000;

void wait_umointor(std::atomic<std::uint32_t>* addr, const std::uint32_t curr) noexcept {
  hw::umonitor(addr);
  if (addr->load(std::memory_order_relaxed) == curr) hw::umwait(0, hw::read_tsc() + BASE_WAIT_TSC);
}

void wait_mointorx(std::atomic<std::uint32_t>* addr, const std::uint32_t curr) noexcept {
  hw::monitorx(addr, 0, 0);
  if (addr->load(std::memory_order_relaxed) == curr) hw::mwaitx(0, 0, BASE_WAIT_TSC);
}

void wait_pause(std::atomic<std::uint32_t>*, std::uint32_t) noexcept { hw::pause(); }

void wait_time_tpause(const std::uint32_t distance) noexcept {
  const std::uint64_t deadline = hw::read_tsc() + distance * BASE_WAIT_TSC;
  hw::tpause(0, deadline);
}

void wait_time_pause(const std::uint32_t distance) noexcept {
  std::uint32_t delay = distance * BASE_PAUSE_LOOPS;
  while (delay--) hw::pause();
}
}  // namespace

namespace detail {
DEFINE_STATIC_CALL(wait_monitor, wait_pause);
REGISTER_STATIC_CALL_ALT_PRIO(wait_monitor, wait_umointor, 200, hw::Feature::WAITPKG);
REGISTER_STATIC_CALL_ALT_PRIO(wait_monitor, wait_mointorx, 100, hw::Feature::MONITORX);

DEFINE_STATIC_CALL(wait_time, wait_time_pause);
REGISTER_STATIC_CALL_ALT(wait_time, wait_time_tpause, hw::Feature::WAITPKG);
}  // namespace detail
}  // namespace kernel::utils