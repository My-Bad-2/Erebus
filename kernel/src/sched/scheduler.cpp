#include "sched/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono.hpp>
#include <utility>

#include "drivers/clock.hpp"
#include "hal/smp/cpu_info.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "sched/balance.hpp"
#include "sched/reaper.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::sched {
namespace {
namespace smp = hw::smp;

constexpr std::uint64_t MIN_SLICE_TIMER_NS = 20'000;
constinit std::atomic g_initialized{false};

[[nodiscard]] std::uint64_t now_ns() noexcept { return drivers::SystemClock::now().raw_ns(); }

hw::timer::TimerAction on_slice_timer(hw::timer::Timer&, void*) noexcept {
  Scheduler::tick();
  return hw::timer::TimerAction::Done;
}

// Keeps the slice timer ahead of the running thread. If it goes off earlier than it has to, it
// finds out what is left of the slice then, and that beats starting a timer on every switch.
void arm_slice_timer(RunQueue& rq, const std::uint64_t now) noexcept {
  if (!rq.wants_timer_locked()) {
    return;
  }

  hw::timer::Timer& timer = rq.slice_timer();
  const klib::TimePoint expiry{now + std::max(rq.slice_left_ns_locked(), MIN_SLICE_TIMER_NS)};

  if (!timer.is_pending() || expiry < timer.deadline()) {
    (void)timer.start_at(expiry);
  }
}

/// Puts a thread nobody else is touching on the queue of @p target, and gets that CPU's attention
/// if it needs it.
///
/// @pre The thread's cpu_id() is @p target already, which was set where the old one allowed it.
void place(Thread* thread, const std::uint32_t target) noexcept {
  utils::IrqDisableGuard irq_guard;

  RunQueue& rq = Scheduler::cpu_runqueue(target);
  const bool local = target == hw::percpu::id();
  const std::uint64_t now = now_ns();
  bool wants_attention = false;

  rq.lock();
  rq.update_clock_locked(now);
  const bool preempts = rq.enqueue_locked(*thread);

  if (local) {
    if (preempts) {
      hw::percpu::request_reschedule();
    }

    arm_slice_timer(rq, now);
  } else {
    // The first thread to wait also means that the CPU has to start its slice timer.
    wants_attention = preempts || rq.waiting() == 1;
  }

  const bool target_busy = rq.current() != rq.idle_thread();
  rq.unlock();

  // An idle CPU is watching the queue that just changed, or will look at it before it sleeps. A
  // busy one is not, and only an interrupt can start its slice timer or take the CPU back.
  if (wants_attention && target_busy) {
    hw::ipi::send_reschedule(target);
  } else if (wants_attention) {
    hw::kick_idle_cpu(target);
  }

  if (target_busy) {
    balance::offer_spare_work(rq);
  }
}
}  // namespace

const auto WAKE_LIST_END = reinterpret_cast<Thread*>(std::uintptr_t{1});

void WakeList::add(Thread& thread) noexcept {
  Thread* expected = nullptr;
  if (!thread.m_wake_next.compare_exchange_strong(expected, WAKE_LIST_END,
                                                  std::memory_order_acq_rel)) {
    return;
  }

  thread.retain();

  if (m_last != nullptr) {
    m_last->m_wake_next.store(&thread, std::memory_order_relaxed);
  } else {
    m_first = &thread;
  }

  m_last = &thread;
}

void WakeList::wake_all() noexcept {
  Thread* thread = m_first;
  m_first = nullptr;
  m_last = nullptr;

  while (thread != nullptr) {
    Thread* next = thread->m_wake_next.load(std::memory_order_relaxed);

    // Off the list first: whoever add() turns away from now on is not covered by this wakeup.
    thread->m_wake_next.store(nullptr, std::memory_order_release);
    (void)Scheduler::wake(thread);
    thread->release();

    thread = next != WAKE_LIST_END ? next : nullptr;
  }
}

void Scheduler::initialize() noexcept {
  balance::build_domains();
  initialize_cpu();
  g_initialized.store(true, std::memory_order_release);
}

void Scheduler::finish_bring_up() noexcept { balance::publish_capacities(); }

void Scheduler::initialize_cpu() noexcept {
  const std::uint32_t cpu = hw::percpu::id();
  const smp::CpuEntry& entry = smp::entry(cpu);

  KASSERT(current() != nullptr);

  RunQueue& rq = hw::percpu::runqueue();
  rq.initialize(cpu, &numa_domain(entry.numa_node), entry.node_slot, current());
  rq.slice_timer().set_callback(&on_slice_timer);

  // Raw for now. What it means depends on what the other CPUs turn out to be.
  const hw::CpuInfo& info = *hw::percpu::self()->info;
  rq.set_capacity(core_performance(info.core_type(), info.max_mhz()));
}

bool Scheduler::is_initialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

RunQueue& Scheduler::cpu_runqueue(const std::uint32_t cpu_id) noexcept {
  return hw::percpu::cpu_area(cpu_id)->runqueue;
}

NumaDomain& Scheduler::numa_domain(const std::uint32_t node_id) noexcept {
  return smp::node_area(node_id)->scheduler;
}

std::uint32_t Scheduler::select_target_cpu(const Thread* thread) noexcept {
  return balance::select_cpu(*thread);
}

auto Scheduler::enqueue(Thread* thread) noexcept -> std::expected<void, Error> {
  if (thread == nullptr || thread->is_idle()) {
    return std::unexpected(Error::InvalidArgument);
  }

  if (!is_initialized()) {
    return std::unexpected(Error::InvalidState);
  }

  const ThreadState state = thread->state();
  if ((state != ThreadState::Ready && state != ThreadState::Created) ||
      !thread->try_hand_to_scheduler()) {
    return std::unexpected(Error::InvalidState);
  }

  // Nobody else knows about it yet, so its cpu_id is free to change.
  const std::uint32_t target = select_target_cpu(thread);
  if (!smp::is_online(target)) {
    thread->revoke_from_scheduler();
    return std::unexpected(Error::InvalidState);
  }

  thread->set_cpu_id(target);
  place(thread, target);
  return {};
}

bool Scheduler::wake(Thread* thread) noexcept {
  utils::IrqDisableGuard irq_guard;

  // It is what runs when nothing else does, so it never leaves. While the system boots it waits
  // for things like anyone else, by going around again, and this is what tells it to stop.
  if (thread->is_idle()) {
    return thread->try_unblock(ThreadState::Running);
  }

  // The queue of the CPU it was last on is the one that knows whether it has really left: that
  // lock is held all the way through a context switch.
  std::uint32_t target_cpu = 0;
  while (true) {
    const std::uint32_t cpu = thread->cpu_id();
    RunQueue& rq = cpu_runqueue(cpu);
    utils::NakedGuard guard{rq};

    if (thread->cpu_id() != cpu) {
      continue;  // Woken by someone else, and run elsewhere since we looked. Look again.
    }

    // Still on its CPU: it announced the block but has not got round to it, and schedule() will
    // see this. It is running, so this has to be one step with whatever it does to its state.
    if (rq.current() == thread) {
      return thread->try_unblock(ThreadState::Running);
    }

    // Off its CPU, which only this lock could change, so nothing else is moving it right now.
    if (!thread->try_unblock(ThreadState::Ready)) {
      return false;
    }

    // Where it goes next is decided here, with the lock still held. Anyone who locks this queue
    // and finds the thread's cpu_id still pointing at it can then be sure it is still here, in
    // whatever state; see Thread::set_cpu_id().
    const std::uint32_t target = select_target_cpu(thread);
    if (!smp::is_online(target)) {
      // Pinned to a CPU that went away. Nothing does that yet; when something does, the thread
      // needs a new home here.
      utils::logger::fatal("sched: thread {} has no CPU to run on\n", thread->tid());
    }

    thread->set_cpu_id(target);
    target_cpu = target;
    break;
  }

  // Other wakers go away now that it is Ready, and nothing else knows about it yet.
  place(thread, target_cpu);
  return true;
}

void Scheduler::apply_priority(Thread& thread, const Priority priority) noexcept {
  if (thread.is_idle()) {
    return;
  }

  utils::IrqDisableGuard irq_guard;
  const std::uint64_t now = now_ns();

  // The same dance as wake(), and one more step: a thread between two queues is not in the state
  // that either of them could work with, and it will not be for long.
  while (true) {
    const std::uint32_t cpu = thread.cpu_id();
    RunQueue& rq = cpu_runqueue(cpu);
    rq.lock();

    if (thread.cpu_id() != cpu || thread.migrating()) {
      rq.unlock();
      hw::cpu_relax();
      continue;
    }

    rq.update_clock_locked(now);
    rq.set_priority_locked(thread, priority);

    const bool local = cpu == hw::percpu::id();
    const bool should_switch = rq.should_switch_locked();
    const bool wants_timer = rq.wants_timer_locked();

    if (local) {
      if (should_switch) {
        hw::percpu::request_reschedule();
      }

      arm_slice_timer(rq, now);
    }

    rq.unlock();

    if (!local && (should_switch || wants_timer)) {
      hw::ipi::send_reschedule(cpu);
    }

    return;
  }
}

void Scheduler::prepare_to_block() noexcept { current()->set_state(ThreadState::Blocked); }

void Scheduler::cancel_block() noexcept { current()->set_state(ThreadState::Running); }

void Scheduler::switch_away(const PickMode mode) noexcept {
  const std::uint64_t flags = hw::irq::save();
  hw::percpu::preempt_disable();

  RunQueue& rq = hw::percpu::runqueue();
  Thread* prev = current();

  rq.lock();
  hw::percpu::clear_reschedule();

  const std::uint64_t now = now_ns();
  rq.update_clock_locked(now);

  Thread* next = rq.pick_next_locked(*prev, mode);
  arm_slice_timer(rq, now);

  if (next == prev) {
    rq.unlock();
    hw::percpu::preempt_enable();
  } else {
    rq.hand_over_lock();
    switch_to(prev, next,
              mode == PickMode::Preempted ? SwitchReason::Preempted : SwitchReason::Voluntary);

    // Some time later, on some CPU, this thread is back. The rq from above may not be ours now.
    finish_switch();
  }

  hw::irq::restore(flags);
}

void Scheduler::finish_switch() noexcept {
  RunQueue& rq = hw::percpu::runqueue();
  if (rq.take_lock_handover()) {
    // The lock was held all the way through the switch, so a thread that died on its way here has
    // left the CPU by now, and nothing will touch its stack again.
    utils::SList<Thread, ReapQueueTag> dead = rq.take_dead_locked();
    rq.unlock();

    reaper::enqueue_dead(dead);
    hw::percpu::preempt_enable();

    // The thread that just lost this CPU may have gone back on the queue.
    balance::offer_spare_work(rq);
  }
}

void Scheduler::schedule() noexcept { switch_away(PickMode::Voluntary); }

void Scheduler::yield() noexcept { switch_away(PickMode::Yield); }

void Scheduler::preempt() noexcept {
  // Threads run by hand are none of our business, and neither is one on its way out: it is about
  // to leave anyway, and putting it back on a queue would take away the fact that it is dead.
  const Thread* self = current();
  if (self == nullptr || self->is_idle() || self != hw::percpu::runqueue().current() ||
      self->state() == ThreadState::Dead) {
    return;
  }

  switch_away(PickMode::Preempted);
}

void Scheduler::handle_ast() noexcept {
  if ((hw::percpu::ast_flags() & hw::percpu::AST_DPC_PENDING) != 0) {
    hw::percpu::self()->dpcs.drain();
  }

  if ((hw::percpu::ast_flags() & hw::percpu::AST_NEED_RESCHED) != 0) {
    preempt();
  }
}

void Scheduler::tick() noexcept {
  RunQueue& rq = hw::percpu::runqueue();
  if (!rq.is_ready()) {
    return;
  }

  const std::uint64_t now = now_ns();
  {
    utils::NakedGuard guard{rq};

    rq.update_clock_locked(now);
    if (rq.should_switch_locked()) {
      hw::percpu::request_reschedule();
    }

    arm_slice_timer(rq, now);
  }

  balance::offer_spare_work(rq);
}

[[noreturn]] void Scheduler::exit_current() noexcept {
  Thread* self = current();

  // Interrupts stay off from here on: this stack is not to be used for anything else now, and
  // the next thing to touch it is whoever destroys it.
  hw::irq::disable();

  if (self != hw::percpu::runqueue().current()) {
    // Run by hand, and its exit hook came back. Whoever did that has to deal with it.
    hw::irq::enable();
    while (true) {
      asm volatile("hlt");
    }
  }

  KASSERT(!self->holds_contended_mutex());

  self->set_state(ThreadState::Dead);
  switch_away(PickMode::Voluntary);
  std::unreachable();
}

void Scheduler::return_to_user() noexcept {
  while (true) {
    // A switch comes back with interrupts still off, and a flag may have gone up meanwhile.
    if (hw::percpu::ast_pending()) {
      handle_ast();
      continue;
    }

    if (current()->exit_requested()) [[unlikely]] {
      exit_current();
    }

    return;
  }
}

void Scheduler::idle_step() noexcept {
  RunQueue& rq = hw::percpu::runqueue();

  if (rq.waiting() == 0) {
    // In this order. Whoever queues a thread reads the idle bitmaps after doing so, and this reads
    // the queue after setting its bit, so a thread that arrives right now is seen by one of us.
    rq.publish_idle();

    if (rq.waiting() == 0 && !balance::steal_work(rq)) {
      // Nobody to take turns with. Whatever the slice timer still has to say, this CPU can do
      // without hearing it, and without being woken for it.
      (void)rq.slice_timer().cancel();
      hw::percpu::timers().prepare_for_idle();

      utils::rcu::note_quiescent_state();
      hw::cpu_idle(rq.idle_word());
      return;
    }
  }

  schedule();
}

[[noreturn]] void Scheduler::enter_idle_loop() noexcept {
  while (true) {
    idle_step();
  }
}
}  // namespace kernel::sched

namespace kernel::hw::percpu {
void preempt_schedule() noexcept {
  if (irq::is_enabled()) {
    irq::disable();
    sched::Scheduler::handle_ast();
    irq::enable();
  }
}
}  // namespace kernel::hw::percpu
