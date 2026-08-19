#include "hal/hal.hpp"
#include "utils/lock.hpp"

namespace kernel::utils {
void TicketSpinlock::lock() noexcept {
  const std::uint32_t my_ticket = m_next_ticket.fetch_add(1, std::memory_order_relaxed);

  while (true) {
    const std::uint32_t curr = m_now_serving.load(std::memory_order_acquire);

    if (curr == my_ticket) [[likely]] {
      return;
    }

    const std::uint32_t distance = my_ticket - curr;

    if (distance == 1) {
      detail::wait_monitor(&m_now_serving, curr);
    } else {
      detail::wait_time(distance);
    }
  }
}

void TicketSpinlock::unlock() noexcept {
  const std::uint32_t curr = m_now_serving.load(std::memory_order_relaxed);
  m_now_serving.store(curr + 1, std::memory_order_release);
}

bool TicketSpinlock::try_lock() noexcept {
  const std::uint32_t curr = m_now_serving.load(std::memory_order_relaxed);
  std::uint32_t expected = curr;

  return m_next_ticket.compare_exchange_strong(expected, curr + 1, std::memory_order_acquire,
                                               std::memory_order_relaxed);
}
} // namespace kernel::utils