// Deferred calls, on the real way out of real interrupts. Everything that makes them useful is
// about the context they run in, so that is what most of these look at.
#include "sched/dpc.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "hal/hal.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/timer/timer.hpp"
#include "ktest.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
namespace percpu = hw::percpu;
using hw::interrupts::Event;
using hw::interrupts::InterruptManager;
using hw::interrupts::InterruptRegistration;
using hw::interrupts::InterruptStatus;
using klib::Duration;
using sched::Dpc;
using support::A_MOMENT;
using support::start;
using support::wait_for;

// Nothing else is registered up here, so tests can have it to themselves.
constexpr std::uint8_t TEST_VECTOR = 0xC8;

// What a call saw of the world it ran in.
struct Report {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<std::uint32_t> cpu{~0u};
  std::atomic<bool> interrupts_on{false};
  std::atomic<bool> in_softirq{false};
  std::atomic<bool> in_hardirq{true};
  std::atomic<bool> could_sleep{true};
};

void report(void* context) {
  auto* seen = static_cast<Report*>(context);

  seen->cpu.store(percpu::id(), std::memory_order_relaxed);
  seen->interrupts_on.store(hw::irq::is_enabled(), std::memory_order_relaxed);
  seen->in_softirq.store(percpu::in_softirq(), std::memory_order_relaxed);
  seen->in_hardirq.store(percpu::in_hardirq(), std::memory_order_relaxed);
  seen->could_sleep.store(percpu::can_sleep(), std::memory_order_relaxed);
  seen->calls.fetch_add(1, std::memory_order_acq_rel);
}

struct Sequence {
  std::array<std::uint32_t, 8> order{};
  std::atomic<std::uint32_t> length{0};
};

struct Step {
  Sequence* sequence;
  std::uint32_t id;
};

void note_step(void* context) {
  const auto* step = static_cast<Step*>(context);
  step->sequence->order[step->sequence->length.fetch_add(1, std::memory_order_acq_rel)] = step->id;
}

struct Repeater {
  Dpc self;
  std::uint32_t left;
  std::atomic<std::uint32_t> calls{0};
};

void go_again(void* context) {
  auto* repeater = static_cast<Repeater*>(context);

  repeater->calls.fetch_add(1, std::memory_order_acq_rel);
  if (repeater->left-- > 1) {
    (void)repeater->self.queue();
  }
}

hw::timer::TimerAction queue_from_interrupt(hw::timer::Timer&, void* dpc) noexcept {
  (void)static_cast<Dpc*>(dpc)->queue();
  return hw::timer::TimerAction::Done;
}

struct Interrupted {
  Report inner{};
  Dpc inner_dpc{&report, &inner};
  hw::timer::Timer timer{&queue_from_interrupt, &inner_dpc};
  std::atomic<std::uint32_t> calls_seen_at_the_end{~0u};
};

// A long call. An interrupt comes along during it, and queues another: that one must wait.
void take_a_while(void* context) {
  auto* interrupted = static_cast<Interrupted*>(context);

  (void)interrupted->timer.start_after(Duration::microseconds(50));
  support::spin_for(Duration::milliseconds(1));

  interrupted->calls_seen_at_the_end.store(interrupted->inner.calls.load(),
                                           std::memory_order_release);
}

struct Exclusive {
  std::atomic<std::uint32_t> inside{0};
  std::atomic<std::uint32_t> overlaps{0};
  std::atomic<std::uint32_t> calls{0};
};

void stay_for_a_moment(void* context) {
  auto* exclusive = static_cast<Exclusive*>(context);

  if (exclusive->inside.fetch_add(1, std::memory_order_acq_rel) != 0) {
    exclusive->overlaps.fetch_add(1, std::memory_order_relaxed);
  }

  for (std::uint32_t i = 0; i < 16; ++i) {
    hw::pause();
  }

  exclusive->inside.fetch_sub(1, std::memory_order_acq_rel);
  exclusive->calls.fetch_add(1, std::memory_order_relaxed);
}

constinit Exclusive g_exclusive;
constinit Dpc g_exclusive_dpc{&stay_for_a_moment, &g_exclusive};

struct SlowCall {
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
};

void be_slow(void* context) {
  auto* slow = static_cast<SlowCall*>(context);

  slow->started.store(true, std::memory_order_release);
  support::spin_for(A_MOMENT);
  slow->finished.store(true, std::memory_order_release);
}

struct Flusher {
  Dpc* dpc{nullptr};
  std::atomic<bool> queued{false};
};

void queue_it_there(Flusher* flusher) noexcept {
  (void)flusher->dpc->queue();
  flusher->queued.store(true, std::memory_order_release);
}

struct Device {
  std::atomic<std::uint32_t> interrupts{0};
  std::atomic<std::uint32_t> calls{0};
  std::atomic<bool> interrupts_on_in_call{false};
};

InterruptStatus device_interrupt(const Event&, void* context) {
  static_cast<Device*>(context)->interrupts.fetch_add(1, std::memory_order_acq_rel);
  return InterruptStatus::ScheduleDPC;
}

void device_call(void* context) {
  auto* device = static_cast<Device*>(context);

  device->interrupts_on_in_call.store(hw::irq::is_enabled(), std::memory_order_relaxed);
  device->calls.fetch_add(1, std::memory_order_acq_rel);
}

void raise_test_interrupt() noexcept { percpu::lapic().send_self_ipi(TEST_VECTOR); }

InterruptStatus defer_everything(const Event&, void*) { return InterruptStatus::ScheduleDPC; }

void raise_it_here(std::atomic<bool>* raised) noexcept {
  raise_test_interrupt();
  raised->store(true, std::memory_order_release);
}
}  // namespace

KTEST(Ast, flags_go_up_and_come_down) {
  const hw::RFlags flags = hw::irq::save();
  percpu::preempt_disable();

  (void)percpu::fetch_clear_ast(0xFF);

  percpu::or_ast(percpu::AST_DPC_PENDING);
  KEXPECT_EQ(percpu::ast_flags(), percpu::AST_DPC_PENDING);

  percpu::or_ast(percpu::AST_NEED_RESCHED);
  KEXPECT_EQ(percpu::fetch_clear_ast(percpu::AST_DPC_PENDING),
             percpu::AST_DPC_PENDING | percpu::AST_NEED_RESCHED);
  KEXPECT_EQ(percpu::ast_flags(), percpu::AST_NEED_RESCHED);

  percpu::clear_reschedule();
  KEXPECT_EQ(percpu::ast_flags(), 0);

  hw::irq::restore(flags);
  percpu::preempt_enable();
}

KTEST(Ast, pending_means_something_can_be_done_about_it) {
  const hw::RFlags flags = hw::irq::save();

  KEXPECT(!percpu::ast_pending());

  percpu::or_ast(percpu::AST_DPC_PENDING);
  KEXPECT(percpu::ast_pending());

  percpu::preempt_disable();
  KEXPECT(!percpu::ast_pending());
  (void)percpu::fetch_clear_ast(percpu::AST_DPC_PENDING);
  percpu::preempt_enable();

  hw::irq::restore(flags);
}

KTEST(Dpc, with_nothing_in_the_way_it_runs_right_away) {
  Report seen;
  Dpc dpc{&report, &seen};

  KEXPECT(dpc.queue());

  KEXPECT_EQ(seen.calls.load(), 1u);
  KEXPECT(!dpc.is_queued());
}

KTEST(Dpc, runs_on_the_cpu_that_asked_for_it) {
  Report seen;
  Dpc dpc{&report, &seen};

  percpu::preempt_disable();
  const std::uint32_t cpu = percpu::id();
  KEXPECT(dpc.queue());
  percpu::preempt_enable();

  KEXPECT_EQ(seen.cpu.load(), cpu);
}

KTEST(Dpc, runs_like_an_interrupt_with_interrupts_on) {
  Report seen;
  Dpc dpc{&report, &seen};

  KEXPECT(dpc.queue());

  KEXPECT(seen.interrupts_on.load());
  KEXPECT(seen.in_softirq.load());
  KEXPECT(!seen.in_hardirq.load());
  KEXPECT(!seen.could_sleep.load());
}

// A lock held with preemption off is safe from these, and that is the reason why.
KTEST(Dpc, waits_for_preemption_to_come_back_on) {
  Report seen;
  Dpc dpc{&report, &seen};

  percpu::preempt_disable();
  KEXPECT(dpc.queue());
  support::spin_for(Duration::microseconds(100));
  KEXPECT_EQ(seen.calls.load(), 0u);
  percpu::preempt_enable();

  KEXPECT_EQ(seen.calls.load(), 1u);
}

// Nothing else is coming on a CPU with no timer running, and no preempt_enable() either. This is
// the one case where it has to go out of its way to get a call to happen.
KTEST(Dpc, gets_going_after_interrupts_come_back_on) {
  Report seen;
  Dpc dpc{&report, &seen};

  hw::irq::disable();
  KEXPECT(dpc.queue());
  hw::irq::enable();

  KEXPECT(wait_for([&] { return seen.calls.load() == 1; }, Duration::seconds(5)));
}

KTEST(Dpc, one_interrupt_after_another_is_one_call) {
  Report seen;
  Dpc dpc{&report, &seen};

  percpu::preempt_disable();
  KEXPECT(dpc.queue());
  KEXPECT(!dpc.queue());
  KEXPECT(!dpc.queue());
  KEXPECT(dpc.is_queued());
  percpu::preempt_enable();

  KEXPECT_EQ(seen.calls.load(), 1u);
}

KTEST(Dpc, calls_run_in_the_order_they_came) {
  Sequence sequence;
  std::array<Step, 3> steps{Step{&sequence, 0}, Step{&sequence, 1}, Step{&sequence, 2}};
  Dpc first{&note_step, &steps[0]};
  Dpc second{&note_step, &steps[1]};
  Dpc third{&note_step, &steps[2]};

  percpu::preempt_disable();
  (void)second.queue();
  (void)third.queue();
  (void)first.queue();
  percpu::preempt_enable();

  KREQUIRE_EQ(sequence.length.load(), 3u);
  KEXPECT_EQ(sequence.order[0], 1u);
  KEXPECT_EQ(sequence.order[1], 2u);
  KEXPECT_EQ(sequence.order[2], 0u);
}

// Queued by an interrupt on a thread that does nothing but spin. With nothing else coming, only the
// way out of that interrupt can get it going.
KTEST(Dpc, runs_on_the_way_out_of_the_interrupt_that_asked) {
  Report seen;
  Dpc dpc{&report, &seen};
  hw::timer::Timer timer{&queue_from_interrupt, &dpc};

  (void)timer.start_after(Duration::microseconds(100));
  KEXPECT(wait_for([&] { return seen.calls.load() == 1; }));

  timer.cancel_sync();
  KEXPECT(seen.interrupts_on.load());
  KEXPECT(seen.in_softirq.load());
  KEXPECT(!seen.in_hardirq.load());
}

KTEST(Dpc, an_interrupt_during_a_call_does_not_run_another_on_top_of_it) {
  Interrupted interrupted;
  Dpc outer{&take_a_while, &interrupted};

  KEXPECT(outer.queue());
  KEXPECT(wait_for([&] { return interrupted.inner.calls.load() == 1; }));

  // It went off in the middle, and had to wait for the end.
  KEXPECT_EQ(interrupted.calls_seen_at_the_end.load(), 0u);
  interrupted.timer.cancel_sync();
}

KTEST(Dpc, queueing_itself_means_running_again) {
  Repeater repeater{.self = Dpc{&go_again, &repeater}, .left = 3};

  KEXPECT(repeater.self.queue());
  KEXPECT(wait_for([&] { return repeater.calls.load() == 3; }));
}

// Something that keeps queueing itself would keep the CPU in here for good. The test passing at all
// means that the CPU came back in between, and each call took just one step of the way.
KTEST(Dpc, cannot_keep_the_cpu_from_its_threads) {
  static constexpr std::uint32_t CALLS = 200;

  Repeater repeater{.self = Dpc{&go_again, &repeater}, .left = CALLS};

  KEXPECT(repeater.self.queue());

  // The call above came back with all the rounds it was allowed, and work left over.
  const std::uint32_t calls_at_once = repeater.calls.load();
  KEXPECT_LT(calls_at_once, CALLS);

  KEXPECT(wait_for([&] { return repeater.calls.load() == CALLS; }));
}

KTEST_ALL_CPUS(Dpc, runs_on_one_cpu_at_a_time, cpu) {
  static constexpr std::uint32_t ROUNDS = 40'000;

  (void)cpu;
  testing::sync_cpus();

  for (std::uint32_t round = 0; round < ROUNDS; ++round) {
    (void)g_exclusive_dpc.queue();
  }

  testing::sync_cpus();
  g_exclusive_dpc.flush();

  KEXPECT_EQ(g_exclusive.overlaps.load(), 0u);
  KEXPECT_GT(g_exclusive.calls.load(), 0u);
  KEXPECT(!g_exclusive_dpc.is_queued());
}

KTEST(Dpc, flush_waits_for_a_call_running_elsewhere) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes another CPU to be running on");
  }

  SlowCall slow;
  Dpc dpc{&be_slow, &slow};
  Flusher flusher{.dpc = &dpc};

  KREQUIRE(start("flusher", &queue_it_there, &flusher, support::pinned_to(support::last_cpu())) !=
           nullptr);
  KREQUIRE(wait_for([&] { return slow.started.load(std::memory_order_acquire); }));

  dpc.flush();
  KEXPECT(slow.finished.load(std::memory_order_acquire));

  // The thread still has a word to write into this frame after the call: that has to be done
  // before the frame goes, or it lands in whatever the next test keeps there.
  KEXPECT(wait_for([&] { return flusher.queued.load(std::memory_order_acquire); }));
}

KTEST(Dpc, flush_of_something_that_is_not_queued_is_over_at_once) {
  Report seen;
  Dpc dpc{&report, &seen};

  dpc.flush();
  KEXPECT_EQ(seen.calls.load(), 0u);
}

KTEST(InterruptManager, an_interrupt_can_put_off_its_work) {
  Device device;
  InterruptRegistration registration{&device_interrupt, &device, &device_call};

  InterruptManager::register_handler(TEST_VECTOR, &registration);

  raise_test_interrupt();
  KEXPECT(wait_for([&] { return device.calls.load() == 1; }));

  KEXPECT_EQ(device.interrupts.load(), 1u);
  KEXPECT(device.interrupts_on_in_call.load());

  InterruptManager::unregister_handler(TEST_VECTOR, &registration);
}

KTEST(InterruptManager, interrupts_that_come_in_a_burst_share_a_call) {
  Device device;
  InterruptRegistration registration{&device_interrupt, &device, &device_call};

  InterruptManager::register_handler(TEST_VECTOR, &registration);

  percpu::preempt_disable();
  for (std::uint32_t i = 1; i <= 8; ++i) {
    raise_test_interrupt();
    KEXPECT(wait_for([&] { return device.interrupts.load() == i; }));
  }

  KEXPECT_EQ(device.calls.load(), 0u);
  percpu::preempt_enable();

  KEXPECT_EQ(device.calls.load(), 1u);
  InterruptManager::unregister_handler(TEST_VECTOR, &registration);
}

KTEST(InterruptManager, handlers_on_one_vector_each_get_their_call) {
  Device first;
  Device second;
  InterruptRegistration one{&device_interrupt, &first, &device_call};
  InterruptRegistration other{&device_interrupt, &second, &device_call};

  InterruptManager::register_handler(TEST_VECTOR, &one);
  InterruptManager::register_handler(TEST_VECTOR, &other);

  raise_test_interrupt();

  KEXPECT(wait_for([&] { return first.calls.load() == 1 && second.calls.load() == 1; }));

  InterruptManager::unregister_handler(TEST_VECTOR, &other);
  InterruptManager::unregister_handler(TEST_VECTOR, &one);
}

KTEST(InterruptManager, a_handler_without_a_call_to_defer_is_just_handled) {
  Device device;
  InterruptRegistration registration{&device_interrupt, &device};

  InterruptManager::register_handler(TEST_VECTOR, &registration);

  raise_test_interrupt();
  KEXPECT(wait_for([&] { return device.interrupts.load() == 1; }));

  KEXPECT_EQ(device.calls.load(), 0u);
  InterruptManager::unregister_handler(TEST_VECTOR, &registration);
}

// A handler that goes away has to take what it deferred with it: the call is in its registration.
KTEST(InterruptManager, unregistering_waits_for_what_was_deferred) {
  Device device;
  InterruptRegistration registration{&device_interrupt, &device, &device_call};

  InterruptManager::register_handler(TEST_VECTOR, &registration);

  percpu::preempt_disable();
  raise_test_interrupt();
  KEXPECT(wait_for([&] { return device.interrupts.load() == 1; }));
  percpu::preempt_enable();

  InterruptManager::unregister_handler(TEST_VECTOR, &registration);
  KEXPECT_EQ(device.calls.load(), 1u);
}

// The same, with the call still going on another CPU, and the handler about to be gone.
KTEST(InterruptManager, unregistering_waits_for_a_call_on_another_cpu) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes another CPU to be running on");
  }

  SlowCall slow;
  std::atomic<bool> raised{false};
  InterruptRegistration registration{&defer_everything, &slow, &be_slow};

  InterruptManager::register_handler(TEST_VECTOR, &registration);

  KREQUIRE(start("raise", &raise_it_here, &raised, support::pinned_to(support::last_cpu())) !=
           nullptr);
  KREQUIRE(wait_for([&] { return slow.started.load(std::memory_order_acquire); }));

  InterruptManager::unregister_handler(TEST_VECTOR, &registration);

  KEXPECT(slow.finished.load(std::memory_order_acquire));
  KEXPECT(wait_for([&] { return raised.load(std::memory_order_acquire); }));
}

}  // namespace kernel::testing
