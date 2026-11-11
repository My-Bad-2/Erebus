#include "sched/dpc.hpp"

#include <chrono.hpp>
#include <utility>

#include "hal/hal.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"

namespace kernel::sched {
namespace {
namespace percpu = hw::percpu;

constexpr klib::Duration RETRY_DELAY = klib::Duration::microseconds(100);

hw::timer::TimerAction retry_pending_calls(hw::timer::Timer&, void*) noexcept {
  percpu::or_ast(percpu::AST_DPC_PENDING);
  return hw::timer::TimerAction::Done;
}
}  // namespace

bool Dpc::queue() noexcept {
  KASSERT(m_callback != nullptr);

  if ((m_state.fetch_or(QUEUED, std::memory_order_acq_rel) & QUEUED) != 0) {
    return false;
  }

  const hw::RFlags flags = hw::irq::save();
  percpu::self()->dpcs.add(*this);
  const bool in_a_thread = !percpu::in_interrupt();
  const bool interrupts_were_off = !flags.get_interrupt_flag();
  const bool nothing_else_will = in_a_thread && interrupts_were_off && percpu::can_sleep();
  hw::irq::restore(flags);

  // On the way out of an interrupt this gets looked at anyway. In a thread, the next interrupt may
  // be a long way off, so go now if that is possible, and make one if not.
  if (nothing_else_will) {
    hw::ipi::send_reschedule(percpu::id());
  } else if (percpu::ast_pending()) {
    percpu::preempt_schedule();
  }

  return true;
}

void Dpc::flush() const noexcept {
  KASSERT(percpu::can_sleep() && hw::irq::is_enabled());

  // If it is waiting here, going through the scheduler lets it run: see preempt_schedule().
  while (m_state.load(std::memory_order_acquire) != 0) {
    Scheduler::yield();
    hw::cpu_relax();
  }
}

// Interrupts are off, so nothing else can get at the list.
void DpcQueue::add(Dpc& dpc) noexcept {
  if (m_last != nullptr) {
    m_pending.insert_after(m_pending.iterator_to(*m_last), dpc);
  } else {
    m_pending.push_front(dpc);
  }

  m_last = &dpc;
  percpu::or_ast(percpu::AST_DPC_PENDING);
}

void DpcQueue::run(Dpc& dpc) noexcept {
  if ((dpc.m_state.fetch_or(Dpc::RUNNING, std::memory_order_acquire) & Dpc::RUNNING) != 0) {
    // Running on another CPU right now, and it is this one's turn after that.
    const hw::RFlags flags = hw::irq::save();
    add(dpc);
    hw::irq::restore(flags);
    return;
  }

  // Before the call. Something that comes in from now on is not covered by what this call sees,
  // so it has to be able to queue the Dpc again.
  dpc.m_state.fetch_and(static_cast<std::uint8_t>(~Dpc::QUEUED), std::memory_order_acq_rel);

  dpc.m_callback(dpc.m_context);
  ++m_calls;

  // Dpc::flush() may be watching this. After it, the object can be gone.
  dpc.m_state.fetch_and(static_cast<std::uint8_t>(~Dpc::RUNNING), std::memory_order_release);
}

void DpcQueue::drain() noexcept {
  percpu::softirq_enter();

  for (std::uint32_t pass = 0; pass < MAX_PASSES; ++pass) {
    (void)percpu::fetch_clear_ast(percpu::AST_DPC_PENDING);

    utils::SList<Dpc> batch = std::move(m_pending);
    m_last = nullptr;

    hw::irq::enable();

    while (!batch.empty()) {
      run(*batch.pop_front());
    }

    hw::irq::disable();

    if ((percpu::ast_flags() & percpu::AST_DPC_PENDING) == 0) {
      percpu::softirq_exit();
      return;
    }
  }

  (void)percpu::fetch_clear_ast(percpu::AST_DPC_PENDING);

  m_retry.set_callback(&retry_pending_calls);
  (void)m_retry.start_after(RETRY_DELAY);

  percpu::softirq_exit();
}
}  // namespace kernel::sched
