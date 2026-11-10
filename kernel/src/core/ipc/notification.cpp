#include "core/ipc/notification.hpp"

#include "core/ipc/endpoint.hpp"
#include "hal/smp/percpu.hpp"
#include "sched/scheduler.hpp"

namespace kernel::core::ipc {
namespace {
using Guard = utils::IrqSaveGuard<utils::QSpinlock>;
}  // namespace

Notification::~Notification() noexcept { KASSERT(m_bound == nullptr); }

void Notification::signal(std::uint64_t bits) noexcept {
  if (bits == 0) [[unlikely]] {
    return;
  }

  // A waiter evaluates its condition after it is on the queue, so either it sees the bits or the
  // wake finds it there.
  m_word.fetch_or(bits, std::memory_order_seq_cst);
  (void)m_waiters.wake_one();

  // The bound thread, if it waits on an endpoint. It is kept until woken, so the binding may go
  // as soon as the lock does.
  hw::percpu::preempt_disable();
  sched::WakeList wakes;
  {
    Guard guard{m_lock};
    if (m_bound != nullptr &&
        m_bound->m_wait.load(std::memory_order_seq_cst) == IpcWait::Receiving) {
      wakes.add(*m_bound->m_thread);
    }
  }

  wakes.wake_all();
  hw::percpu::preempt_enable();
}

std::expected<std::uint64_t, Error> Notification::wait() noexcept {
  std::uint64_t bits = 0;
  m_waiters.wait([&] {
    bits = m_word.exchange(0, std::memory_order_acq_rel);
    return bits != 0 || m_closed.load(std::memory_order_acquire);
  });

  return bits != 0 ? std::expected<std::uint64_t, Error>{bits}
                   : std::unexpected(Error::InvalidState);
}

std::expected<std::uint64_t, Error> Notification::wait_for(klib::Duration timeout) noexcept {
  std::uint64_t bits = 0;
  const bool done = m_waiters.wait_for(timeout, [&] {
    bits = m_word.exchange(0, std::memory_order_acq_rel);
    return bits != 0 || m_closed.load(std::memory_order_acquire);
  });

  if (bits != 0) {
    return bits;
  }

  return std::unexpected(done ? Error::InvalidState : Error::Timeout);
}

std::expected<void, Error> Notification::bind(ThreadIpcContext& ctx) noexcept {
  Guard guard{m_lock};
  const Notification* current = ctx.m_bound.load(std::memory_order_relaxed);
  if ((m_bound != nullptr && m_bound != &ctx) || (current != nullptr && current != this))
      [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  if (m_bound == nullptr) {
    add_ref();
    m_bound = &ctx;
    ctx.m_bound.store(this, std::memory_order_release);
  }

  return {};
}

void Notification::unbind(ThreadIpcContext& ctx) noexcept {
  {
    Guard guard{m_lock};
    if (m_bound != &ctx) {
      return;
    }

    m_bound = nullptr;
    ctx.m_bound.store(nullptr, std::memory_order_release);
  }

  // Readers of ctx.m_bound look in a read section, so the memory waits for them.
  release_ref();
}

void Notification::on_unreachable() noexcept {
  m_closed.store(true, std::memory_order_release);
  (void)m_waiters.wake_all();
}
}  // namespace kernel::core::ipc
