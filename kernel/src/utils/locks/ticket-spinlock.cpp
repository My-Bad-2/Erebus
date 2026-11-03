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

    // Wrap-safe: tickets are handed out in order, so the distance is never "negative".
    const std::uint32_t distance = my_ticket - curr;
    if (distance == 1) {
      detail::wait_monitor(&m_now_serving, curr);
    } else {
      detail::wait_time(distance);
    }
  }
}

void TicketSpinlock::unlock() noexcept {
  // Only the owner writes this counter, so no read-modify-write is needed.
  m_now_serving.store(m_now_serving.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

bool TicketSpinlock::try_lock() noexcept {
  // Free means nobody holds or waits for a ticket: take the next one only if it is served now.
  std::uint32_t ticket = m_now_serving.load(std::memory_order_relaxed);
  return m_next_ticket.compare_exchange_strong(ticket, ticket + 1, std::memory_order_acquire,
                                               std::memory_order_relaxed);
}
}  // namespace kernel::utils
