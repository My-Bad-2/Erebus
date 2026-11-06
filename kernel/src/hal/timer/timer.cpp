#include "hal/timer/timer.hpp"

#include <algorithm>
#include <utility>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::timer {
namespace {
constexpr auto TIMER_VECTOR = std::to_underlying(interrupts::InterruptVector::LocalApicTimer);

[[nodiscard]] std::uint64_t clock_ns() noexcept { return drivers::SystemClock::now().raw_ns(); }

interrupts::InterruptStatus on_timer_interrupt(const interrupts::Event&, void*) noexcept {
  percpu::timers().on_interrupt();
  return interrupts::InterruptStatus::Handled;
}

constinit interrupts::InterruptRegistration g_timer_handler{&on_timer_interrupt, nullptr};

// A timer that became the first on another CPU's queue: only that CPU can reach its hardware.
void reprogram_here(void*) noexcept { percpu::timers().reprogram(); }
}  // namespace

bool Timer::start_at(const klib::TimePoint deadline, const klib::Duration slack) noexcept {
  utils::IrqDisableGuard irq_guard;
  return percpu::timers().add(*this, deadline, slack);
}

bool Timer::start_after(const klib::Duration delay, const klib::Duration slack) noexcept {
  return start_at(drivers::SystemClock::now() + delay, slack);
}

auto Timer::start_on(const std::uint32_t cpu, const klib::TimePoint deadline,
                     const klib::Duration slack) noexcept -> std::expected<bool, Error> {
  if (!smp::is_online(cpu)) {
    return std::unexpected(Error::InvalidState);
  }

  return percpu::cpu_area(cpu)->timers.add(*this, deadline, slack);
}

bool Timer::cancel() noexcept {
  while (true) {
    TimerQueue* queue = m_queue.load(std::memory_order_acquire);
    if (queue == nullptr) {
      return false;
    }

    utils::IrqSaveGuard guard{*queue};

    // It can have moved in the meantime. This queue's lock only means something if it is still
    // here.
    if (m_queue.load(std::memory_order_acquire) == queue) {
      queue->unlink_locked(*this);
      m_queue.store(nullptr, std::memory_order_release);
      return true;
    }
  }
}

void Timer::cancel_sync() noexcept {
  while (true) {
    (void)cancel();

    const TimerQueue* running = m_running_on.load(std::memory_order_acquire);
    if (running == nullptr) {
      // Looked at in this order, as a callback that returns Restart is back on the queue before it
      // stops counting as running.
      if (m_queue.load(std::memory_order_acquire) == nullptr) {
        return;
      }

      continue;
    }

    // Callbacks run with interrupts off, so one on this CPU is the one that called this.
    if (running == &percpu::timers()) {
      return;
    }

    cpu_relax();
  }
}

std::uint64_t Timer::forward(const klib::Duration period, const klib::TimePoint now) noexcept {
  const auto step = static_cast<std::uint64_t>(period.to_ns());
  if (step == 0) {
    return 0;
  }

  const std::uint64_t time = now.raw_ns();
  const std::uint64_t periods = time >= m_deadline_ns ? (time - m_deadline_ns) / step + 1 : 1;

  m_deadline_ns += periods * step;
  return periods;
}

std::uint64_t Timer::forward(const klib::Duration period) noexcept {
  return forward(period, drivers::SystemClock::now());
}

void TimerQueue::attach(const std::uint32_t cpu, const std::uint64_t limit_ns) noexcept {
  utils::IrqSaveGuard guard{*this};

  m_cpu = cpu;
  m_hardware_limit_ns = limit_ns;
  m_has_hardware = limit_ns != 0;
}

void TimerQueue::limit_hardware_wait(const std::uint64_t limit_ns) noexcept {
  utils::IrqSaveGuard guard{*this};
  m_hardware_limit_ns = limit_ns;
}

// In this order for ties: a timer goes behind those that were there first, so `m_first` only
// changes when the new one really is earlier.
void TimerQueue::link_locked(Timer& timer) noexcept {
  const std::uint64_t slack = timer.m_precision == TimerPrecision::Exact ? 0 : timer.m_slack_ns;
  const std::uint64_t advance =
      timer.m_precision == TimerPrecision::Exact && m_has_hardware ? m_latency.advance_ns() : 0;

  // An Exact timer goes off early and spins. Anything else may go any time in its slack, and the
  // hardware waits for the end of that: something else may yet come along to share the interrupt.
  timer.m_fire_ns = timer.m_deadline_ns + slack - std::min(advance, timer.m_deadline_ns);
  timer.m_earliest_ns =
      timer.m_precision == TimerPrecision::Exact ? timer.m_fire_ns : timer.m_deadline_ns;

  m_tree.insert(timer);
  if (m_first == nullptr || timer.m_fire_ns < m_first->m_fire_ns) {
    m_first = &timer;
  }
}

void TimerQueue::unlink_locked(Timer& timer) noexcept {
  if (m_first == &timer) {
    m_first = Tree::next(timer);
  }

  m_tree.erase(timer);
}

void TimerQueue::reprogram_locked() noexcept {
  if (!m_has_hardware || m_expiring || m_first == nullptr) {
    return;
  }

  // Already going off in time? Then it finds out what is next at that point.
  const std::uint64_t target_ns = m_first->m_fire_ns;
  if (target_ns >= m_armed_ns) {
    return;
  }

  // One reading of the clock, so that the two sides of the conversion agree. How far off it can
  // be depends on how far ahead the timer is, however long the machine has been running.
  const std::uint64_t raw = drivers::SystemClock::raw_ticks();
  const std::uint64_t now = drivers::SystemClock::raw_to_ns(raw);
  const std::uint64_t delay = std::min(target_ns > now ? target_ns - now : 0, m_hardware_limit_ns);

  m_armed_ns = now + delay;
  ++m_stats.hardware_arms;
  percpu::lapic().set_timer(raw, delay);
}

void TimerQueue::reprogram() noexcept {
  utils::IrqSaveGuard guard{*this};
  reprogram_locked();
}

bool TimerQueue::add(Timer& timer, const klib::TimePoint deadline,
                     const klib::Duration slack) noexcept {
  KASSERT(timer.m_callback != nullptr);

  utils::IrqDisableGuard irq_guard;
  bool was_waiting = false;

  // Claiming the timer for this queue under its lock is what keeps two of these from both putting
  // it somewhere: whoever loses goes around, takes it off again, and has the last word.
  while (true) {
    was_waiting |= timer.cancel();

    lock();

    TimerQueue* expected = nullptr;
    if (timer.m_queue.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
      break;
    }

    unlock();
  }

  timer.m_deadline_ns = deadline.raw_ns();
  timer.m_slack_ns = static_cast<std::uint64_t>(std::max<std::int64_t>(slack.to_ns(), 0));
  link_locked(timer);

  const bool local = !m_has_hardware || this == &percpu::timers();
  const bool remote_kick = !local && m_first == &timer;

  if (local) {
    reprogram_locked();
  }

  unlock();

  if (remote_kick) {
    (void)ipi::call(m_cpu, &reprogram_here, nullptr, ipi::Wait::No);
  }

  return was_waiting;
}

// Starts out without the lock, which a callback has to be free of, and ends up holding it.
void TimerQueue::run(Timer& timer, std::uint64_t& now_ns) noexcept {
  if (m_has_hardware) {
    if (timer.m_precision == TimerPrecision::Exact) {
      // Bounded by the advance it went off with, which has an upper limit. No PAUSE in here: it
      // can take longer than the rest of the loop, and every cycle is a part of the error.
      while (now_ns < timer.m_deadline_ns) {
        now_ns = clock_ns();
      }
    }

    if (now_ns > timer.m_deadline_ns) {
      m_stats.max_late_ns = std::max(m_stats.max_late_ns, now_ns - timer.m_deadline_ns);
    }
  }

  const TimerAction action = timer.m_callback(timer, timer.m_context);

  lock();

  // The callback may have started the timer itself, in which case that is what counts.
  TimerQueue* expected = nullptr;
  if (action == TimerAction::Restart &&
      timer.m_queue.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
    link_locked(timer);
  }

  // After this, the timer can be gone: cancel_sync() is what its owner waits on.
  timer.m_running_on.store(nullptr, std::memory_order_release);
}

std::uint32_t TimerQueue::expire(std::uint64_t now_ns) noexcept {
  std::uint32_t expired = 0;

  lock();
  m_expiring = true;

  while (expired < MAX_EXPIRIES_PER_PASS) {
    Timer* timer = m_first;
    if (timer == nullptr || timer->m_earliest_ns > now_ns) {
      break;
    }

    unlink_locked(*timer);
    timer->m_queue.store(nullptr, std::memory_order_release);
    timer->m_running_on.store(this, std::memory_order_release);
    unlock();

    run(*timer, now_ns);

    ++expired;
    ++m_stats.expired;

    if (m_has_hardware) {
      now_ns = clock_ns();
    }
  }

  m_expiring = false;
  reprogram_locked();
  unlock();

  return expired;
}

void TimerQueue::on_interrupt() noexcept {
  const std::uint64_t now = clock_ns();

  lock();

  const std::uint64_t armed_for = m_armed_ns;
  m_armed_ns = NOT_ARMED;

  if (armed_for != NOT_ARMED && now >= armed_for) {
    m_latency.observe(now - armed_for);
  }

  unlock();

  if (expire(now) == 0) {
    ++m_stats.idle_wakeups;
  }
}

bool TimerQueue::is_consistent() const noexcept {
  if (!m_tree.is_consistent() || m_first != m_tree.first()) {
    return false;
  }

  for (const Timer* timer = m_first; timer != nullptr; timer = Tree::next(*timer)) {
    if (timer->m_queue.load(std::memory_order_relaxed) != this) {
      return false;
    }
  }

  return true;
}

void initialize() noexcept {
  interrupts::InterruptManager::register_handler(TIMER_VECTOR, &g_timer_handler);

  const apic::Lapic& lapic = percpu::lapic();
  if (lapic.uses_tsc_deadline()) {
    utils::logger::info("Timers: TSC deadline\n");
  } else {
    utils::logger::info("Timers: APIC one-shot at {} kHz\n", lapic.timer_hz() / 1000);
  }
}

void initialize_cpu() noexcept {
  const apic::Lapic& lapic = percpu::lapic();
  const std::uint64_t limit_ns = lapic.max_timer_delay_ns();

  if (limit_ns == 0) {
    utils::logger::warn("Timers: no working timer on CPU {}, nothing will go off\n", percpu::id());
    return;
  }

  lapic.configure_timer(TIMER_VECTOR);
  percpu::timers().attach(percpu::id(), limit_ns);
}
}  // namespace kernel::hw::timer
