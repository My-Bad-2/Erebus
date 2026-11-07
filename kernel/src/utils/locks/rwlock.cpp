#include "utils/locks/locks.hpp"

namespace kernel::utils {
namespace {
constexpr std::uint32_t WRITER_LOCKED = 1 << 0;
constexpr std::uint32_t WRITER_WAITING = 1 << 1;
constexpr std::uint32_t WRITER_MASK = WRITER_LOCKED | WRITER_WAITING;
constexpr std::uint32_t READER_INC = 1 << 8;
}  // namespace

void RWLock::read_lock() noexcept {
  while (true) {
    std::uint32_t state = m_rw_state.fetch_add(READER_INC, std::memory_order_acquire);

    // No writer inside or waiting: we are in.
    if ((state & WRITER_MASK) == 0) [[likely]] {
      return;
    }

    // A writer holds or waits for the lock. Undo and wait for it to finish.
    state = m_rw_state.fetch_sub(READER_INC, std::memory_order_relaxed) - READER_INC;
    while (state & WRITER_MASK) {
      detail::wait_monitor(&m_rw_state, state);
      state = m_rw_state.load(std::memory_order_relaxed);
    }
  }
}

void RWLock::read_unlock() noexcept { m_rw_state.fetch_sub(READER_INC, std::memory_order_release); }

void RWLock::write_lock() noexcept {
  m_write_queue.lock();

  // Announce the writer without touching the reader count: readers already inside must still
  // be counted, new ones back off as soon as they see the bit.
  m_rw_state.fetch_or(WRITER_WAITING, std::memory_order_relaxed);

  // Readers that bump the count and see the bit undo it again, so the count can blip while we
  // wait. Only a CAS from "waiting, no readers" is a safe hand-over.
  while (true) {
    std::uint32_t expected = WRITER_WAITING;
    if (m_rw_state.compare_exchange_strong(expected, WRITER_LOCKED, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
      return;
    }

    detail::wait_monitor(&m_rw_state, expected);
  }
}

void RWLock::write_unlock() noexcept {
  // Not a plain store: a reader bouncing off right now has its increment in the word too.
  m_rw_state.fetch_and(~WRITER_LOCKED, std::memory_order_release);
  m_write_queue.unlock();
}
}  // namespace kernel::utils