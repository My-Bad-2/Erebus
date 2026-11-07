// Threads that wait and threads that wake them, on real CPUs. What makes these hard to get right is
// what the two can do at the same moment, so most of the time here goes to making that happen.
#include "sched/wait_queue.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "hal/timer/timer.hpp"
#include "ktest.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using sched::Thread;
using sched::ThreadState;
using sched::WaitQueue;
using support::A_MOMENT;
using support::start;
using support::wait_for;

// Waits for a number to go up to what it wants, then says so.
struct Waiter {
  WaitQueue* queue{nullptr};
  std::atomic<std::uint32_t>* gate{nullptr};
  std::uint32_t wanted{1};
  std::atomic<std::uint32_t>* tickets{nullptr};
  std::atomic<std::uint32_t> ticket{~0u};
  std::atomic<bool> done{false};
};

void wait_on_the_gate(Waiter* waiter) noexcept {
  waiter->queue->wait(
      [waiter] { return waiter->gate->load(std::memory_order_acquire) >= waiter->wanted; });

  if (waiter->tickets != nullptr) {
    waiter->ticket.store(waiter->tickets->fetch_add(1, std::memory_order_acq_rel),
                         std::memory_order_release);
  }

  waiter->done.store(true, std::memory_order_release);
}

struct TimedWaiter {
  WaitQueue* queue{nullptr};
  std::atomic<bool>* condition{nullptr};
  Duration timeout{};
  std::atomic<bool> result{false};
  std::atomic<std::uint64_t> took_ns{0};
  std::atomic<bool> done{false};
};

void wait_with_a_limit(TimedWaiter* waiter) noexcept {
  const klib::TimePoint before = drivers::SystemClock::now();

  const bool result = waiter->queue->wait_for(
      waiter->timeout, [waiter] { return waiter->condition->load(std::memory_order_acquire); });

  waiter->took_ns.store((drivers::SystemClock::now() - before).to_ns(), std::memory_order_release);
  waiter->result.store(result, std::memory_order_release);
  waiter->done.store(true, std::memory_order_release);
}

// Two threads handing a turn back and forth, each only ever waiting for the other.
struct Rally {
  WaitQueue queues[2]{};
  std::atomic<std::uint32_t> turn{0};
  std::uint32_t rounds{0};
  std::atomic<std::uint32_t> done{0};
};

template <std::uint32_t Side>
void play(Rally* rally) noexcept {
  for (std::uint32_t round = 0; round < rally->rounds; ++round) {
    const std::uint32_t mine = round * 2 + Side;

    rally->queues[Side].wait([&] { return rally->turn.load(std::memory_order_acquire) == mine; });

    rally->turn.store(mine + 1, std::memory_order_release);
    (void)rally->queues[1 - Side].wake_one();
  }

  rally->done.fetch_add(1, std::memory_order_acq_rel);
}

// Comes and goes all the time, for wakes to hit at the worst moment. They stop by themselves: the
// test could not tell them to on one CPU, where it only gets a turn once none of them wants one.
struct Churn {
  WaitQueue queue{};
  klib::TimePoint until{};
  std::atomic<std::uint32_t> running{0};
  std::atomic<std::uint64_t> waits{0};
};

void wait_and_time_out(Churn* churn) noexcept {
  while (drivers::SystemClock::now() < churn->until) {
    const std::uint64_t wait = churn->waits.fetch_add(1, std::memory_order_relaxed);

    (void)churn->queue.wait_for(Duration::microseconds(1 + wait % 50), [] { return false; });
  }

  churn->running.fetch_sub(1, std::memory_order_acq_rel);
}

// Starts a thread that lives through one wait and is gone, while wakes are flying.
void wake_like_mad(Churn* churn) noexcept {
  while (drivers::SystemClock::now() < churn->until) {
    (void)churn->queue.wake(2);
    hw::pause();
  }

  churn->running.fetch_sub(1, std::memory_order_acq_rel);
}

struct BootWaker {
  WaitQueue* queue{nullptr};
  std::atomic<bool>* ready{nullptr};
  std::atomic<bool> done{false};
};

void wake_the_boot_thread(BootWaker* waker) noexcept {
  waker->ready->store(true, std::memory_order_release);
  (void)waker->queue->wake_all();
  waker->done.store(true, std::memory_order_release);
}

hw::timer::TimerAction open_the_gate(hw::timer::Timer&, void* payload) noexcept {
  auto* waiter = static_cast<Waiter*>(payload);

  waiter->gate->fetch_add(1, std::memory_order_acq_rel);
  (void)waiter->queue->wake_all();
  return hw::timer::TimerAction::Done;
}
}  // namespace

KTEST(WaitQueue, nothing_to_wake_on_an_empty_queue) {
  WaitQueue queue;

  KEXPECT_EQ(queue.wake_one(), 0u);
  KEXPECT_EQ(queue.wake_all(), 0u);
  KEXPECT_EQ(queue.waiters(), 0u);
}

KTEST(WaitQueue, a_condition_that_holds_does_not_wait) {
  WaitQueue queue;
  bool ran = false;

  queue.wait([&] {
    ran = true;
    return true;
  });

  KEXPECT(ran);
  KEXPECT_EQ(queue.waiters(), 0u);
  KEXPECT(sched::current()->state() == ThreadState::Running);
}

KTEST(WaitQueue, a_wake_gets_a_thread_going) {
  WaitQueue queue;
  std::atomic<std::uint32_t> gate{0};
  Waiter waiter{.queue = &queue, .gate = &gate};

  KREQUIRE(start("wq", &wait_on_the_gate, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return queue.waiters() == 1; }));
  KEXPECT(!waiter.done.load());

  gate.store(1, std::memory_order_release);
  KEXPECT_EQ(queue.wake_all(), 1u);

  KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(queue.waiters(), 0u);
}

KTEST(WaitQueue, a_wake_without_the_condition_sends_them_back) {
  WaitQueue queue;
  std::atomic<std::uint32_t> gate{0};
  Waiter waiter{.queue = &queue, .gate = &gate};

  KREQUIRE(start("wq", &wait_on_the_gate, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return queue.waiters() == 1; }));

  KEXPECT_EQ(queue.wake_all(), 1u);
  KEXPECT(wait_for([&] { return queue.waiters() == 1; }));
  KEXPECT(!waiter.done.load());

  gate.store(1, std::memory_order_release);
  (void)queue.wake_all();
  KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
}

// Each waits for a number of its own, as a thread on the queue that has yet to look at its
// condition goes by what it sees, and needs no wake to leave.
KTEST(WaitQueue, waiters_leave_in_the_order_they_came) {
  static constexpr std::uint32_t WAITERS = 4;

  WaitQueue queue;
  std::atomic<std::uint32_t> gate{0};
  std::atomic<std::uint32_t> tickets{0};
  std::array<Waiter, WAITERS> waiters;

  const std::uint32_t cpu = support::last_cpu();
  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    waiters[i].queue = &queue;
    waiters[i].gate = &gate;
    waiters[i].wanted = i + 1;
    waiters[i].tickets = &tickets;

    KREQUIRE(start("wq", &wait_on_the_gate, &waiters[i], support::pinned_to(cpu)) != nullptr);
    KREQUIRE(wait_for([&] { return queue.waiters() == i + 1; }));
  }

  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    gate.store(i + 1, std::memory_order_release);

    KEXPECT_EQ(queue.wake_one(), 1u);
    KEXPECT(wait_for([&] { return waiters[i].done.load(std::memory_order_acquire); }));
    KEXPECT_EQ(waiters[i].ticket.load(), i);
  }

  KEXPECT_EQ(queue.wake_one(), 0u);
}

// A waiter that is on the queue but has yet to look at its condition goes by what it sees, and
// needs no wake. So the one that has to stay behind waits for more than the others.
KTEST(WaitQueue, wake_takes_as_many_as_it_is_told) {
  static constexpr std::uint32_t WAITERS = 4;

  WaitQueue queue;
  std::atomic<std::uint32_t> gate{1};
  std::array<Waiter, WAITERS> waiters;

  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    waiters[i].queue = &queue;
    waiters[i].gate = &gate;
    waiters[i].wanted = i < WAITERS - 1 ? 2 : 3;
    KREQUIRE(start("wq", &wait_on_the_gate, &waiters[i]) != nullptr);
    KREQUIRE(wait_for([&] { return queue.waiters() == i + 1; }));
  }

  gate.store(2, std::memory_order_release);

  KEXPECT_EQ(queue.wake(3), 3u);
  KEXPECT(wait_for(
      [&] { return waiters[0].done.load() && waiters[1].done.load() && waiters[2].done.load(); }));
  KEXPECT(!waiters[3].done.load());
  KEXPECT_EQ(queue.waiters(), 1u);

  gate.store(3, std::memory_order_release);
  KEXPECT_EQ(queue.wake_all(), 1u);
  KEXPECT(wait_for([&] { return waiters[3].done.load(std::memory_order_acquire); }));
}

KTEST(WaitQueue, a_wait_with_a_limit_gives_up_at_the_limit) {
  WaitQueue queue;
  std::atomic<bool> condition{false};
  TimedWaiter waiter{
      .queue = &queue, .condition = &condition, .timeout = Duration::milliseconds(5)};

  KREQUIRE(start("wq", &wait_with_a_limit, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));

  KEXPECT(!waiter.result.load());
  KEXPECT_GE(waiter.took_ns.load(), static_cast<std::uint64_t>(waiter.timeout.to_ns()));
  KEXPECT_EQ(queue.waiters(), 0u);
}

KTEST(WaitQueue, a_wait_with_a_limit_ends_with_the_condition) {
  WaitQueue queue;
  std::atomic<bool> condition{false};
  TimedWaiter waiter{.queue = &queue, .condition = &condition, .timeout = Duration::seconds(60)};

  KREQUIRE(start("wq", &wait_with_a_limit, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return queue.waiters() == 1; }));

  condition.store(true, std::memory_order_release);
  (void)queue.wake_one();

  KREQUIRE(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  KEXPECT(waiter.result.load());
  KEXPECT_LT(waiter.took_ns.load(), static_cast<std::uint64_t>(Duration::seconds(30).to_ns()));
}

// A wake can be spent on a thread that is on its way out. Its last look at the condition is what
// keeps that from getting lost, and it comes after it found the time was up. With a deadline that
// has passed, the third look is that one: before the wait, on the way to sleep, and then this.
KTEST(WaitQueue, a_timeout_with_the_condition_met_is_not_a_timeout) {
  WaitQueue queue;
  std::uint32_t looks = 0;

  KEXPECT(queue.wait_until(drivers::SystemClock::now() - Duration::seconds(1),
                           [&] { return ++looks == 3; }));
  KEXPECT_EQ(looks, 3u);
}

// On this CPU, so it only gets to run because the boot thread left it, and that is the one moment a
// wake finds a boot thread that is neither running nor anywhere on a queue.
KTEST(WaitQueue, a_thread_can_wake_the_boot_thread_from_its_own_cpu) {
  WaitQueue queue;
  std::atomic<bool> ready{false};
  BootWaker waker{.queue = &queue, .ready = &ready};

  KREQUIRE(start("wq", &wake_the_boot_thread, &waker, support::pinned_to(hw::percpu::id())) !=
           nullptr);

  queue.wait([&] { return ready.load(std::memory_order_acquire); });

  KEXPECT(sched::current()->state() == ThreadState::Running);
  KEXPECT(wait_for([&] { return waker.done.load(std::memory_order_acquire); }));
}

KTEST(WaitQueue, an_interrupt_handler_can_wake_a_thread) {
  WaitQueue queue;
  std::atomic<std::uint32_t> gate{0};
  Waiter waiter{.queue = &queue, .gate = &gate};

  KREQUIRE(start("wq", &wait_on_the_gate, &waiter) != nullptr);
  KREQUIRE(wait_for([&] { return queue.waiters() == 1; }));

  hw::timer::Timer timer{&open_the_gate, &waiter};
  (void)timer.start_after(Duration::milliseconds(1));

  KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  timer.cancel_sync();
}

// Everything on one thread would pass without the wakeups ever being needed, so this makes them
// cross CPUs at full speed. A lost wakeup leaves both sides waiting for the other one forever.
KTEST(WaitQueue, no_wakeup_gets_lost_between_two_cpus) {
  Rally rally;
  rally.rounds = hw::smp::online_cpus() > 1 ? 20'000 : 2'000;

  KREQUIRE(start("wq-a", &play<0>, &rally, support::pinned_to(0)) != nullptr);
  KREQUIRE(start("wq-b", &play<1>, &rally, support::pinned_to(support::last_cpu())) != nullptr);

  KEXPECT(wait_for([&] { return rally.done.load(std::memory_order_acquire) == 2; }));
}

KTEST(WaitQueue, comes_through_timeouts_and_wakes_at_the_same_moment) {
  static constexpr std::uint32_t WAITERS = 6;
  static constexpr std::uint32_t WAKERS = 2;

  Churn churn;
  churn.until = drivers::SystemClock::now() + Duration::milliseconds(50);
  churn.running.store(WAITERS + WAKERS, std::memory_order_release);

  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    KREQUIRE(start("wq-wait", &wait_and_time_out, &churn) != nullptr);
  }

  for (std::uint32_t i = 0; i < WAKERS; ++i) {
    KREQUIRE(start("wq-wake", &wake_like_mad, &churn) != nullptr);
  }

  KEXPECT(wait_for([&] { return churn.running.load(std::memory_order_acquire) == 0; }));
  KEXPECT_EQ(churn.queue.waiters(), 0u);
  KEXPECT_GT(churn.waits.load(), 0ull);
}
}  // namespace kernel::testing
