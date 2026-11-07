// The list that lets a waker get out of the way first, and what keeps a thread around for it.
#include <atomic>
#include <cstdint>

#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "sched/scheduler.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using sched::Scheduler;
using sched::Thread;
using sched::ThreadState;
using sched::WakeList;
using support::A_MOMENT;
using support::start;
using support::wait_for;

// Goes to sleep and notes down when it wakes up.
struct Blocker {
  std::atomic<Thread*> self{nullptr};
  std::atomic<bool> let_go{false};
  std::atomic<std::uint32_t>* next_ticket{nullptr};
  std::atomic<std::uint32_t> ticket{~0u};
  std::atomic<bool> done{false};
};

void block_once(Blocker* blocker) noexcept {
  blocker->self.store(sched::current(), std::memory_order_release);

  while (true) {
    Scheduler::prepare_to_block();
    if (blocker->let_go.load(std::memory_order_acquire)) {
      Scheduler::cancel_block();
      break;
    }

    Scheduler::schedule();
  }

  if (blocker->next_ticket != nullptr) {
    blocker->ticket.store(blocker->next_ticket->fetch_add(1, std::memory_order_acq_rel),
                          std::memory_order_release);
  }

  blocker->done.store(true, std::memory_order_release);
}

[[nodiscard]] bool is_blocked(const Blocker& blocker) noexcept {
  const Thread* thread = blocker.self.load(std::memory_order_acquire);
  return thread != nullptr && thread->state() == ThreadState::Blocked;
}

void do_nothing(void*) noexcept {}

// Holds on to a thread while the test tries to destroy it.
struct Retainer {
  Thread* thread{nullptr};
  std::atomic<bool> holding{false};
  std::atomic<bool> released{false};
};

void hold_for_a_while(Retainer* retainer) noexcept {
  retainer->thread->retain();
  retainer->holding.store(true, std::memory_order_release);

  support::spin_for(A_MOMENT);

  retainer->released.store(true, std::memory_order_release);
  retainer->thread->release();
}
}  // namespace

KTEST(WakeList, an_empty_list_wakes_nothing) {
  hw::percpu::preempt_disable();
  WakeList wakes;
  wakes.wake_all();
  hw::percpu::preempt_enable();
}

// Real-time threads, since those run in the order they became runnable. Ordinary ones would go by
// their deadlines, which says nothing about the order of the wakeups.
KTEST(WakeList, wakes_in_the_order_they_were_added) {
  std::atomic<std::uint32_t> tickets{0};
  Blocker blockers[3];

  const std::uint32_t cpu = support::last_cpu();
  for (Blocker& blocker : blockers) {
    blocker.next_ticket = &tickets;
    KREQUIRE(start("wl-order", &block_once, &blocker,
                   support::of_class(sched::PriorityClass::RealTime, cpu)) != nullptr);
  }

  KREQUIRE(wait_for([&] {
    return is_blocked(blockers[0]) && is_blocked(blockers[1]) && is_blocked(blockers[2]);
  }));

  for (Blocker& blocker : blockers) {
    blocker.let_go.store(true, std::memory_order_release);
  }

  hw::percpu::preempt_disable();
  {
    WakeList wakes;
    wakes.add(*blockers[2].self.load());
    wakes.add(*blockers[0].self.load());
    wakes.add(*blockers[1].self.load());
    wakes.wake_all();
  }
  hw::percpu::preempt_enable();

  KREQUIRE(wait_for([&] {
    return blockers[0].done.load() && blockers[1].done.load() && blockers[2].done.load();
  }));

  KEXPECT_EQ(blockers[2].ticket.load(), 0u);
  KEXPECT_EQ(blockers[0].ticket.load(), 1u);
  KEXPECT_EQ(blockers[1].ticket.load(), 2u);
}

// A thread on two lists would have two successors, which it has one link for.
KTEST(WakeList, a_thread_goes_on_one_list_at_a_time) {
  Blocker blocker;
  KREQUIRE(start("wl-once", &block_once, &blocker, support::pinned_to(support::last_cpu())) !=
           nullptr);
  KREQUIRE(wait_for([&] { return is_blocked(blocker); }));

  blocker.let_go.store(true, std::memory_order_release);

  hw::percpu::preempt_disable();
  {
    WakeList first;
    WakeList second;
    Thread* thread = blocker.self.load();

    first.add(*thread);
    first.add(*thread);
    second.add(*thread);
    second.wake_all();

    // The second list is out of it, so only the first one can get it going.
    support::spin_for(klib::Duration::milliseconds(1));
    KEXPECT(thread->state() == ThreadState::Blocked);

    first.wake_all();
  }
  hw::percpu::preempt_enable();

  KEXPECT(wait_for([&] { return blocker.done.load(); }));
}

KTEST(Thread, destroying_a_thread_waits_for_whoever_retained_it) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("needs a second CPU");
  }

  const auto created = Thread::create_kernel("retained", &do_nothing);
  KREQUIRE(created.has_value());

  Retainer retainer{.thread = *created};
  KREQUIRE(start("retainer", &hold_for_a_while, &retainer,
                 support::pinned_to(support::last_cpu())) != nullptr);
  KREQUIRE(wait_for([&] { return retainer.holding.load(std::memory_order_acquire); }));

  KEXPECT(Thread::destroy(*created).has_value());
  KEXPECT(retainer.released.load(std::memory_order_acquire));
}
}  // namespace kernel::testing
