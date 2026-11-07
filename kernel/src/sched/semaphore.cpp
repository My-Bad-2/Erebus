#include "sched/semaphore.hpp"

#include "utils/assert.hpp"

namespace kernel::sched {
bool Semaphore::try_acquire() noexcept {
  std::ptrdiff_t count = m_count.load(std::memory_order_relaxed);

  while (count > 0) {
    if (m_count.compare_exchange_weak(count, count - 1, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
      return true;
    }
  }

  return false;
}

// A wake goes to one thread, which may already be leaving with another token, or with none because
// it timed out. Tokens would then sit here with someone asleep next to them, so whoever
// gets out of line looks around first.
void Semaphore::pass_on() noexcept {
  if (m_count.load(std::memory_order_acquire) > 0) {
    (void)m_waiters.wake_one();
  }
}

void Semaphore::acquire() noexcept {
  if (try_acquire()) {
    return;
  }

  m_waiters.wait([this] { return try_acquire(); });
  pass_on();
}

bool Semaphore::try_acquire_for(const klib::Duration timeout) noexcept {
  if (try_acquire()) {
    return true;
  }

  const bool acquired = m_waiters.wait_for(timeout, [this] { return try_acquire(); });
  pass_on();
  return acquired;
}

void Semaphore::release(const std::ptrdiff_t count) noexcept {
  KASSERT(count > 0);

  m_count.fetch_add(count, std::memory_order_release);
  (void)m_waiters.wake(static_cast<std::uint32_t>(count));
}
}  // namespace kernel::sched
