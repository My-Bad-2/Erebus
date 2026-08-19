#include "utils/lock.hpp"

namespace kernel::utils {
namespace {
constexpr uint32_t WRITER_LOCKED = 1 << 0;
constexpr uint32_t WRITER_WAITING = 1 << 1;
constexpr uint32_t READER_INC = 1 << 8;
constexpr uint32_t READER_MASK = 0xFFFFFF00;
} // namespace

void RWLock::read_lock() noexcept {
  while (true) {
    std::uint32_t state = m_rw_state.fetch_add(READER_INC, std::memory_order_acquire);

    // If no writer is waiting and no write is locked, we are safe to proceed.
    if ((state & (WRITER_LOCKED | WRITER_WAITING)) == 0) [[likely]] {
      return;
    }

    // A write holds or is waiting for the lock. Undo and wait.
    m_rw_state.fetch_sub(READER_INC, std::memory_order_relaxed);

    state = m_rw_state.load(std::memory_order_relaxed);
    while (state & (WRITER_LOCKED | WRITER_WAITING)) {
      detail::wait_monitor(&m_rw_state, state);
      state = m_rw_state.load(std::memory_order_relaxed);
    }
  }
}

void RWLock::read_unlock() noexcept { m_rw_state.fetch_sub(READER_INC, std::memory_order_release); }

void RWLock::write_lock() noexcept {
  m_write_queue.lock();

  m_rw_state.store(WRITER_WAITING, std::memory_order_relaxed);
  while (true) {
    const std::uint32_t state = m_rw_state.load(std::memory_order_acquire);
    if ((state & READER_MASK) == 0) {
      m_rw_state.store(WRITER_LOCKED, std::memory_order_release);
      return;
    }

    detail::wait_monitor(&m_rw_state, state);
  }
}

void RWLock::write_unlock() noexcept {
  m_rw_state.store(0, std::memory_order_release);
  m_write_queue.unlock();
}
} // namespace kernel::utils