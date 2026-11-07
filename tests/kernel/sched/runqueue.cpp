// A run queue that belongs to no CPU, filled with threads that never run: the decisions it takes,
// with nothing else going on. No other CPU can see these queues, so the lock is left alone.
#include "sched/runqueue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "sched/numa_domain.hpp"
#include "sched/thread.hpp"

namespace kernel::testing {
namespace {
using sched::CpuPlacement;
using sched::NumaDomain;
using sched::PickMode;
using sched::Priority;
using sched::PriorityClass;
using sched::RunQueue;
using sched::Thread;
using sched::ThreadCreateOptions;
using sched::ThreadState;

void do_nothing(void*) noexcept {}

// A fresh queue with an idle thread of its own, and the threads to go on it.
class Fixture {
  static constexpr std::size_t MAX_THREADS = 16;

  std::array<Thread*, MAX_THREADS> m_threads{};
  std::size_t m_count{0};
  Thread* m_idle{nullptr};

 public:
  RunQueue queue;

  explicit Fixture(NumaDomain* domain = nullptr) noexcept {
    const auto idle =
        Thread::create_kernel("rq-idle", &do_nothing, nullptr, {.priority = PriorityClass::Idle});
    m_idle = idle ? *idle : nullptr;
    queue.initialize(0, domain, 0, m_idle);
  }

  ~Fixture() noexcept {
    for (std::size_t i = 0; i < m_count; ++i) {
      (void)queue.remove_locked(*m_threads[i]);
      m_threads[i]->set_state(ThreadState::Dead);
      (void)Thread::destroy(m_threads[i]);
    }

    (void)Thread::destroy(m_idle);
  }

  [[nodiscard]] Thread* idle() const noexcept { return m_idle; }

  [[nodiscard]] Thread* make(const ThreadCreateOptions& opts = {}) noexcept {
    const auto created = Thread::create_kernel("rq", &do_nothing, nullptr, opts);
    if (!created) {
      return nullptr;
    }

    m_threads[m_count++] = *created;
    return *created;
  }

  [[nodiscard]] Thread* make(const PriorityClass priority) noexcept {
    return make(ThreadCreateOptions{.priority = priority});
  }

  // Forgets a thread that something else has taken care of, such as the dead list.
  void disown(const Thread* thread) noexcept {
    for (std::size_t i = 0; i < m_count; ++i) {
      if (m_threads[i] == thread) {
        m_threads[i] = m_threads[--m_count];
        return;
      }
    }
  }

  // What a CPU does after the decision: the chosen thread becomes the one that is running.
  Thread* run_next(Thread& prev, const PickMode mode = PickMode::Voluntary) noexcept {
    Thread* next = queue.pick_next_locked(prev, mode);
    next->set_state(ThreadState::Running);
    return next;
  }
};
}  // namespace

KTEST(RunQueue, runs_the_idle_thread_when_there_is_nothing_else) {
  Fixture fixture;
  KREQUIRE(fixture.idle() != nullptr);

  KEXPECT(fixture.queue.current() == fixture.idle());
  KEXPECT(fixture.queue.pick_next_locked(*fixture.idle()) == fixture.idle());
  KEXPECT_EQ(fixture.queue.waiting(), 0u);
  KEXPECT_EQ(fixture.queue.load(), 0u);
  KEXPECT(!fixture.queue.should_switch_locked());
}

KTEST(RunQueue, goes_back_to_idle_when_the_last_thread_blocks) {
  Fixture fixture;
  Thread* thread = fixture.make();
  KREQUIRE(thread != nullptr);

  fixture.queue.enqueue_locked(*thread);
  KEXPECT_EQ(fixture.queue.load(), 1u);

  KEXPECT(fixture.run_next(*fixture.idle()) == thread);
  KEXPECT(fixture.queue.current() == thread);
  KEXPECT_EQ(fixture.queue.load(), 1u);
  KEXPECT(!thread->on_runqueue());

  thread->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*thread) == fixture.idle());
  KEXPECT(fixture.queue.current() == fixture.idle());
  KEXPECT_EQ(fixture.queue.load(), 0u);
  KEXPECT_EQ(fixture.queue.fair().total_weight(), 0u);
}

KTEST(RunQueue, says_when_a_new_thread_should_take_over) {
  Fixture fixture;
  Thread* fair = fixture.make();
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  Thread* runner = fixture.make(PriorityClass::TestRunner);
  Thread* later = fixture.make(PriorityClass::RealTime);
  Thread* another_fair = fixture.make();
  KREQUIRE(fair && realtime && runner && later && another_fair);

  // Anything beats the idle thread.
  KEXPECT(fixture.queue.enqueue_locked(*fair));
  KEXPECT(fixture.run_next(*fixture.idle()) == fair);

  // Real-time beats fair, and a higher level beats a lower one.
  KEXPECT(fixture.queue.enqueue_locked(*realtime));
  KEXPECT(fixture.run_next(*fair) == realtime);
  KEXPECT(fixture.queue.enqueue_locked(*runner));
  KEXPECT(fixture.run_next(*realtime, PickMode::Preempted) == runner);

  // But not the other way around, and an equal does not.
  KEXPECT(!fixture.queue.enqueue_locked(*later));
  KEXPECT(!fixture.queue.enqueue_locked(*another_fair));
}

KTEST(RunQueue, real_time_threads_go_first_in_the_order_they_came) {
  Fixture fixture;
  Thread* fair = fixture.make();
  Thread* first = fixture.make(PriorityClass::RealTime);
  Thread* second = fixture.make(PriorityClass::RealTime);
  Thread* third = fixture.make(PriorityClass::RealTime);
  KREQUIRE(fair && first && second && third);

  fixture.queue.enqueue_locked(*fair);
  fixture.queue.enqueue_locked(*first);
  fixture.queue.enqueue_locked(*second);
  fixture.queue.enqueue_locked(*third);

  KEXPECT(fixture.run_next(*fixture.idle()) == first);

  first->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*first) == second);

  second->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*second) == third);

  third->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*third) == fair);
}

KTEST(RunQueue, the_higher_real_time_level_goes_first) {
  Fixture fixture;
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  Thread* runner = fixture.make(PriorityClass::TestRunner);
  KREQUIRE(realtime && runner);

  fixture.queue.enqueue_locked(*realtime);
  fixture.queue.enqueue_locked(*runner);

  KEXPECT(fixture.run_next(*fixture.idle()) == runner);
}

KTEST(RunQueue, a_real_time_thread_keeps_the_cpu_from_its_equals_until_it_yields) {
  Fixture fixture;
  Thread* running = fixture.make(PriorityClass::RealTime);
  Thread* next = fixture.make(PriorityClass::RealTime);
  KREQUIRE(running && next);

  fixture.queue.enqueue_locked(*running);
  KEXPECT(fixture.run_next(*fixture.idle()) == running);

  fixture.queue.enqueue_locked(*next);
  KEXPECT(fixture.queue.pick_next_locked(*running) == running);
  KEXPECT(fixture.queue.pick_next_locked(*running, PickMode::Preempted) == running);
  KEXPECT(!fixture.queue.should_switch_locked());

  KEXPECT(fixture.run_next(*running, PickMode::Yield) == next);
  KEXPECT(running->on_runqueue());
  KEXPECT(running->state() == ThreadState::Ready);
}

KTEST(RunQueue, a_real_time_thread_is_not_held_up_by_fair_ones) {
  Fixture fixture;
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  Thread* fair = fixture.make();
  KREQUIRE(realtime && fair);

  fixture.queue.enqueue_locked(*realtime);
  KEXPECT(fixture.run_next(*fixture.idle()) == realtime);

  fixture.queue.enqueue_locked(*fair);
  KEXPECT(fixture.queue.pick_next_locked(*realtime, PickMode::Yield) == realtime);
}

KTEST(RunQueue, a_preempted_real_time_thread_keeps_its_place_in_line) {
  Fixture fixture;
  Thread* preempted = fixture.make(PriorityClass::RealTime);
  Thread* waiting = fixture.make(PriorityClass::RealTime);
  Thread* runner = fixture.make(PriorityClass::TestRunner);
  KREQUIRE(preempted && waiting && runner);

  fixture.queue.enqueue_locked(*preempted);
  KEXPECT(fixture.run_next(*fixture.idle()) == preempted);

  fixture.queue.enqueue_locked(*waiting);
  fixture.queue.enqueue_locked(*runner);
  KEXPECT(fixture.run_next(*preempted, PickMode::Preempted) == runner);

  runner->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*runner) == preempted);
}

KTEST(RunQueue, one_that_yields_goes_to_the_end_of_the_line) {
  Fixture fixture;
  Thread* yielder = fixture.make(PriorityClass::RealTime);
  Thread* second = fixture.make(PriorityClass::RealTime);
  Thread* third = fixture.make(PriorityClass::RealTime);
  KREQUIRE(yielder && second && third);

  fixture.queue.enqueue_locked(*yielder);
  KEXPECT(fixture.run_next(*fixture.idle()) == yielder);

  fixture.queue.enqueue_locked(*second);
  fixture.queue.enqueue_locked(*third);
  KEXPECT(fixture.run_next(*yielder, PickMode::Yield) == second);

  second->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*second) == third);

  third->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*third) == yielder);
}

KTEST(RunQueue, a_fair_thread_keeps_running_while_nothing_is_more_deserving) {
  Fixture fixture;
  Thread* running = fixture.make();
  Thread* patient = fixture.make(ThreadCreateOptions{.slice_ns = 64 * sched::DEFAULT_SLICE_NS});
  KREQUIRE(running && patient);

  fixture.queue.enqueue_locked(*running);
  KEXPECT(fixture.run_next(*fixture.idle()) == running);

  fixture.queue.enqueue_locked(*patient);
  KEXPECT(fixture.queue.pick_next_locked(*running) == running);
  KEXPECT(fixture.queue.current() == running);
  KEXPECT_EQ(fixture.queue.waiting(), 1u);
}

KTEST(RunQueue, a_fair_thread_that_used_up_its_turn_makes_way) {
  Fixture fixture;
  Thread* first = fixture.make();
  Thread* second = fixture.make();
  KREQUIRE(first && second);

  fixture.queue.update_clock_locked(1000);
  fixture.queue.enqueue_locked(*first);
  fixture.queue.enqueue_locked(*second);
  KEXPECT(fixture.run_next(*fixture.idle()) == first);

  fixture.queue.update_clock_locked(1000 + 2 * sched::DEFAULT_SLICE_NS);
  KEXPECT(fixture.queue.should_switch_locked());
  KEXPECT(fixture.run_next(*first, PickMode::Preempted) == second);

  KEXPECT(first->on_runqueue());
  KEXPECT(first->state() == ThreadState::Ready);
  KEXPECT(!second->on_runqueue());
  KEXPECT_EQ(fixture.queue.waiting(), 1u);
}

KTEST(RunQueue, a_thread_caught_on_its_way_to_blocking_stays_runnable) {
  Fixture fixture;
  Thread* blocking = fixture.make();
  Thread* other = fixture.make();
  KREQUIRE(blocking && other);

  fixture.queue.enqueue_locked(*blocking);
  KEXPECT(fixture.run_next(*fixture.idle()) == blocking);
  fixture.queue.enqueue_locked(*other);

  // It announced the block, and then an interrupt took the CPU away before it got to schedule().
  // Taking that for the real thing would lose it if the wakeup has already been and gone.
  blocking->set_state(ThreadState::Blocked);
  const std::uint64_t weight = fixture.queue.fair().total_weight();
  (void)fixture.queue.pick_next_locked(*blocking, PickMode::Preempted);

  KEXPECT_EQ(fixture.queue.fair().total_weight(), weight);
}

KTEST(RunQueue, a_blocked_thread_leaves_the_competition) {
  Fixture fixture;
  Thread* blocking = fixture.make();
  Thread* other = fixture.make();
  KREQUIRE(blocking && other);

  fixture.queue.enqueue_locked(*blocking);
  fixture.queue.enqueue_locked(*other);
  KEXPECT(fixture.run_next(*fixture.idle()) == blocking);

  blocking->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*blocking) == other);

  KEXPECT(!blocking->on_runqueue());
  KEXPECT_EQ(fixture.queue.waiting(), 0u);
  KEXPECT_EQ(fixture.queue.fair().total_weight(), other->weight());
}

KTEST(RunQueue, a_dead_thread_is_kept_until_it_can_be_destroyed) {
  Fixture fixture;
  Thread* dying = fixture.make();
  KREQUIRE(dying != nullptr);

  fixture.queue.enqueue_locked(*dying);
  KEXPECT(fixture.run_next(*fixture.idle()) == dying);

  dying->set_state(ThreadState::Dead);
  KEXPECT(fixture.run_next(*dying) == fixture.idle());

  fixture.disown(dying);
  KEXPECT_EQ(fixture.queue.drain_dead(), 1u);
  KEXPECT_EQ(fixture.queue.drain_dead(), 0u);
}

KTEST(RunQueue, the_fair_tier_and_the_counts_stay_in_step) {
  Fixture fixture;
  std::array<Thread*, 8> threads{};
  for (Thread*& thread : threads) {
    thread = fixture.make();
    KREQUIRE(thread != nullptr);
    fixture.queue.enqueue_locked(*thread);
  }

  KEXPECT_EQ(fixture.queue.waiting(), 8u);
  KEXPECT_EQ(fixture.queue.fair().queued(), 8u);

  Thread* prev = fixture.idle();
  for (std::uint32_t round = 0; round < 64; ++round) {
    fixture.queue.update_clock_locked((round + 1) * sched::DEFAULT_SLICE_NS);
    prev = fixture.run_next(*prev, PickMode::Preempted);
    KEXPECT(fixture.queue.fair().is_consistent());
    KEXPECT_EQ(fixture.queue.waiting(), 7u);
  }

  prev->set_state(ThreadState::Blocked);
  (void)fixture.queue.pick_next_locked(*prev);
}

KTEST(RunQueue, threads_can_be_taken_off_from_anywhere_in_the_line) {
  Fixture fixture;
  Thread* first = fixture.make(PriorityClass::RealTime);
  Thread* middle = fixture.make(PriorityClass::RealTime);
  Thread* last = fixture.make(PriorityClass::RealTime);
  Thread* newcomer = fixture.make(PriorityClass::RealTime);
  KREQUIRE(first && middle && last && newcomer);

  fixture.queue.enqueue_locked(*first);
  fixture.queue.enqueue_locked(*middle);
  fixture.queue.enqueue_locked(*last);

  KEXPECT(fixture.queue.remove_locked(*middle));
  KEXPECT(!fixture.queue.remove_locked(*middle));
  KEXPECT(!middle->on_runqueue());

  // Taking off the tail: a new thread has to go behind what is the last one now.
  KEXPECT(fixture.queue.remove_locked(*last));
  fixture.queue.enqueue_locked(*newcomer);
  KEXPECT_EQ(fixture.queue.waiting(), 2u);

  KEXPECT(fixture.run_next(*fixture.idle()) == first);
  first->set_state(ThreadState::Blocked);
  KEXPECT(fixture.run_next(*first) == newcomer);
}

KTEST(RunQueue, a_waiting_fair_thread_can_be_taken_off) {
  Fixture fixture;
  Thread* thread = fixture.make();
  KREQUIRE(thread != nullptr);

  KEXPECT(!fixture.queue.remove_locked(*thread));

  fixture.queue.enqueue_locked(*thread);
  KEXPECT(fixture.queue.remove_locked(*thread));
  KEXPECT_EQ(fixture.queue.waiting(), 0u);
  KEXPECT_EQ(fixture.queue.fair().total_weight(), 0u);
}

KTEST(RunQueue, destroying_a_queued_thread_is_refused) {
  Fixture fixture;
  Thread* thread = fixture.make();
  KREQUIRE(thread != nullptr);

  fixture.queue.enqueue_locked(*thread);

  const auto destroyed = Thread::destroy(thread);
  KREQUIRE(!destroyed.has_value());
  KEXPECT(destroyed.error() == Error::InvalidState);
}

KTEST(RunQueue, the_slice_timer_is_only_for_taking_turns) {
  Fixture fixture;
  Thread* first = fixture.make();
  Thread* second = fixture.make();
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  Thread* waiting = fixture.make(PriorityClass::RealTime);
  KREQUIRE(first && second && realtime && waiting);

  KEXPECT(!fixture.queue.wants_timer_locked());

  fixture.queue.enqueue_locked(*first);
  KEXPECT(fixture.run_next(*fixture.idle()) == first);
  KEXPECT(!fixture.queue.wants_timer_locked());

  fixture.queue.enqueue_locked(*second);
  KEXPECT(fixture.queue.wants_timer_locked());

  // Real-time threads do not take turns, so nothing has to interrupt them.
  fixture.queue.enqueue_locked(*realtime);
  KEXPECT(fixture.run_next(*first) == realtime);
  fixture.queue.enqueue_locked(*waiting);
  KEXPECT(!fixture.queue.wants_timer_locked());
}

KTEST(RunQueue, thieves_leave_pinned_threads_alone) {
  Fixture fixture;
  Thread* pinned = fixture.make(ThreadCreateOptions{.preferred_cpu = 0, .pinned = true});
  KREQUIRE(pinned != nullptr);

  fixture.queue.enqueue_locked(*pinned);
  KEXPECT(fixture.queue.try_steal(1, 0) == nullptr);

  // With the latest deadline, so that it is the first one a thief looks at, and has to pass over.
  Thread* free = fixture.make();
  Thread* pinned_late = fixture.make(ThreadCreateOptions{
      .preferred_cpu = 0, .slice_ns = 8 * sched::DEFAULT_SLICE_NS, .pinned = true});
  KREQUIRE(free && pinned_late);

  fixture.queue.enqueue_locked(*free);
  fixture.queue.enqueue_locked(*pinned_late);

  KEXPECT(fixture.queue.try_steal(1, 0) == free);
  KEXPECT(!free->on_runqueue());
  KEXPECT_EQ(fixture.queue.waiting(), 2u);
  KEXPECT(fixture.queue.try_steal(1, 0) == nullptr);
}

KTEST(RunQueue, thieves_can_ask_for_a_queue_with_threads_to_spare) {
  Fixture fixture;
  Thread* first = fixture.make();
  Thread* second = fixture.make();
  KREQUIRE(first && second);

  fixture.queue.enqueue_locked(*first);
  KEXPECT(fixture.queue.try_steal(2, 0) == nullptr);

  fixture.queue.enqueue_locked(*second);
  KEXPECT(fixture.queue.try_steal(2, 0) != nullptr);
}

// Far enough away, a thread is only worth taking from someone it would otherwise never get a
// turn from.
KTEST(RunQueue, a_thread_behind_a_real_time_one_is_worth_coming_a_long_way_for) {
  Fixture fixture;
  Thread* fair = fixture.make();
  Thread* other = fixture.make();
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  KREQUIRE(fair && other && realtime);

  fixture.queue.enqueue_locked(*fair);
  fixture.queue.enqueue_locked(*other);
  KEXPECT(fixture.run_next(*fixture.idle()) == fair);

  // Taking turns with another fair thread: it will get its go.
  KEXPECT_EQ(fixture.queue.spare(), 1u);
  KEXPECT(fixture.queue.try_steal(2, 0) == nullptr);

  fixture.queue.enqueue_locked(*realtime);
  KEXPECT(fixture.run_next(*fair) == realtime);

  // Now there are two waiting behind something that does not take turns.
  KEXPECT_EQ(fixture.queue.spare(), 3u);
  KEXPECT(fixture.queue.try_steal(2, 0) != nullptr);
  KEXPECT(fixture.queue.try_steal(2, 0) != nullptr);

  // Nobody left to help, and no reason to come.
  KEXPECT_EQ(fixture.queue.spare(), 0u);
  KEXPECT(fixture.queue.try_steal(1, 0) == nullptr);
}

KTEST(RunQueue, thieves_take_the_thread_with_the_longest_to_wait) {
  Fixture fixture;
  Thread* soon = fixture.make();
  Thread* late = fixture.make(ThreadCreateOptions{.slice_ns = 8 * sched::DEFAULT_SLICE_NS});
  Thread* realtime = fixture.make(PriorityClass::RealTime);
  KREQUIRE(soon && late && realtime);

  fixture.queue.enqueue_locked(*soon);
  fixture.queue.enqueue_locked(*late);
  KEXPECT(fixture.queue.try_steal(1, 0) == late);

  fixture.queue.enqueue_locked(*realtime);
  KEXPECT(fixture.queue.try_steal(1, 0) == realtime);
}

KTEST(RunQueue, thieves_give_up_rather_than_search_a_long_queue) {
  Fixture fixture;

  // Nine pinned threads in front of one that could go: more than a thief looks at, and the point
  // of the limit is that the victim is not kept waiting.
  for (std::uint32_t i = 0; i < 9; ++i) {
    Thread* pinned = fixture.make(ThreadCreateOptions{
        .priority = PriorityClass::RealTime, .preferred_cpu = 0, .pinned = true});
    KREQUIRE(pinned != nullptr);
    fixture.queue.enqueue_locked(*pinned);
  }

  Thread* free = fixture.make(PriorityClass::RealTime);
  KREQUIRE(free != nullptr);
  fixture.queue.enqueue_locked(*free);

  KEXPECT(fixture.queue.try_steal(1, 0) == nullptr);
}

// Virtual time on two CPUs has nothing to do with each other, so a thread cannot carry its virtual
// runtime over. What it carries is how far it was from fair.
KTEST(RunQueue, a_stolen_thread_keeps_what_it_is_owed) {
  Fixture from;
  Fixture to;
  Thread* running = from.make();
  Thread* owed = from.make();
  Thread* resident = to.make();
  KREQUIRE(running && owed && resident);

  from.queue.update_clock_locked(1000);
  from.queue.enqueue_locked(*running);
  from.queue.enqueue_locked(*owed);
  KEXPECT(from.run_next(*from.idle()) == running);
  from.queue.update_clock_locked(1000 + sched::DEFAULT_SLICE_NS / 2);

  // Has not had its turn yet, so it is owed time.
  KEXPECT(from.queue.try_steal(1, 0) == owed);
  const std::int64_t lag = owed->vlag();
  KEXPECT_GT(lag, 0);

  // The other queue is at a very different virtual time, and has someone to compare it with.
  to.queue.update_clock_locked(1000);
  to.queue.enqueue_locked(*resident);
  KEXPECT(to.run_next(*to.idle()) == resident);
  to.queue.update_clock_locked(1000 + 100 * sched::DEFAULT_SLICE_NS);

  to.queue.enqueue_locked(*owed);
  KEXPECT(to.queue.fair().is_eligible(*owed));
  KEXPECT(to.queue.remove_locked(*owed));

  const std::int64_t difference = owed->vlag() - lag;
  KEXPECT_LE(difference, 2);
  KEXPECT_GE(difference, -2);

  running->set_state(ThreadState::Blocked);
  (void)from.queue.pick_next_locked(*running);
  resident->set_state(ThreadState::Blocked);
  (void)to.queue.pick_next_locked(*resident);
}

// Nobody claimed this CPU, it found its own work, so nobody else can tell that it is taken.
KTEST(RunQueue, a_cpu_stops_being_idle_when_it_starts_to_run_something) {
  NumaDomain domain;
  const std::array<CpuPlacement, 1> cpus{CpuPlacement{.cpu = 42, .core = 0}};
  KREQUIRE(domain.initialize(0, cpus, {}).has_value());

  {
    Fixture fixture{&domain};
    Thread* thread = fixture.make();
    KREQUIRE(thread != nullptr);

    fixture.queue.publish_idle();
    KEXPECT_EQ(domain.idle_cpu_count(), 1u);

    fixture.queue.enqueue_locked(*thread);
    KEXPECT(fixture.run_next(*fixture.idle()) == thread);
    KEXPECT_EQ(domain.idle_cpu_count(), 0u);
  }

  domain.release();
}

KTEST(RunQueue, tells_the_domain_when_it_has_work_to_spare) {
  NumaDomain domain;
  const std::array<CpuPlacement, 1> cpus{CpuPlacement{.cpu = 42, .core = 0}};
  KREQUIRE(domain.initialize(0, cpus, {}).has_value());

  const auto overloaded = [&domain] {
    bool found = false;
    domain.for_each_overloaded(0, [&found](std::uint32_t) {
      found = true;
      return true;
    });

    return found;
  };

  {
    Fixture fixture{&domain};
    Thread* pinned = fixture.make(ThreadCreateOptions{.preferred_cpu = 0, .pinned = true});
    Thread* free = fixture.make();
    KREQUIRE(pinned && free);

    fixture.queue.enqueue_locked(*pinned);
    KEXPECT(!overloaded());

    fixture.queue.enqueue_locked(*free);
    KEXPECT(overloaded());

    KEXPECT(fixture.queue.remove_locked(*free));
    KEXPECT(!overloaded());
  }

  domain.release();
}

KTEST(RunQueue, a_waiting_thread_can_change_its_share) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::High));

  KEXPECT(thread->priority_class() == PriorityClass::High);
  KEXPECT_EQ(thread->weight(), sched::priority_to_weight(PriorityClass::High));
  KEXPECT_EQ(fixture.queue.waiting(), 1u);
  KEXPECT(thread->on_runqueue());
  KEXPECT(fixture.queue.fair().is_consistent());
  KEXPECT_EQ(fixture.queue.fair().total_weight(),
             std::uint64_t{sched::priority_to_weight(PriorityClass::High)});
}

KTEST(RunQueue, a_waiting_thread_can_move_up_to_the_real_time_tier) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  Thread* other = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr && other != nullptr);

  (void)fixture.queue.enqueue_locked(*other);
  (void)fixture.queue.enqueue_locked(*thread);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::RealTime));

  KEXPECT(!thread->is_queued());
  KEXPECT_EQ(fixture.queue.waiting(), 2u);
  KEXPECT(fixture.run_next(*fixture.idle()) == thread);
  KEXPECT(fixture.queue.fair().is_consistent());
}

KTEST(RunQueue, a_waiting_thread_can_move_down_from_the_real_time_tier) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::RealTime);
  Thread* other = fixture.make(PriorityClass::RealTime);
  KREQUIRE(thread != nullptr && other != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  (void)fixture.queue.enqueue_locked(*other);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::Normal));

  KEXPECT(thread->is_queued());
  KEXPECT_EQ(fixture.queue.waiting(), 2u);
  KEXPECT(fixture.run_next(*fixture.idle()) == other);
}

KTEST(RunQueue, a_waiting_thread_can_change_levels_within_the_real_time_tier) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::RealTime);
  Thread* other = fixture.make(PriorityClass::RealTime);
  KREQUIRE(thread != nullptr && other != nullptr);

  (void)fixture.queue.enqueue_locked(*other);
  (void)fixture.queue.enqueue_locked(*thread);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::TestRunner));

  KEXPECT(fixture.run_next(*fixture.idle()) == thread);
}

KTEST(RunQueue, a_running_fair_thread_can_change_its_share) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  KREQUIRE(fixture.run_next(*fixture.idle()) == thread);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::High));

  KEXPECT(fixture.queue.fair().current() == thread);
  KEXPECT_EQ(thread->weight(), sched::priority_to_weight(PriorityClass::High));
  KEXPECT(fixture.queue.fair().is_consistent());
}

KTEST(RunQueue, a_running_fair_thread_can_become_real_time) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  Thread* other = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr && other != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  KREQUIRE(fixture.run_next(*fixture.idle()) == thread);
  (void)fixture.queue.enqueue_locked(*other);

  const std::uint32_t spare_before = fixture.queue.spare();
  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::RealTime));

  KEXPECT(fixture.queue.fair().current() == nullptr);
  KEXPECT(fixture.queue.current() == thread);
  KEXPECT_EQ(fixture.queue.fair().total_weight(), std::uint64_t{sched::NICE_0_WEIGHT});

  // The one behind it is stuck now, and that has to show, as nothing would come and get it.
  KEXPECT_EQ(fixture.queue.spare(), spare_before + 1);

  // A real-time thread does not lose the CPU to what is only fair.
  KEXPECT(!fixture.queue.should_switch_locked());
  KEXPECT(fixture.run_next(*thread, PickMode::Preempted) == thread);
  KEXPECT(fixture.queue.fair().is_consistent());
}

KTEST(RunQueue, a_running_real_time_thread_can_go_back_to_being_fair) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::RealTime);
  KREQUIRE(thread != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  KREQUIRE(fixture.run_next(*fixture.idle()) == thread);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::Normal));

  KEXPECT(fixture.queue.fair().current() == thread);
  KEXPECT(fixture.queue.current() == thread);
  KEXPECT_EQ(fixture.queue.fair().total_weight(), std::uint64_t{sched::NICE_0_WEIGHT});
  KEXPECT_EQ(fixture.queue.spare(), 0u);
  KEXPECT(fixture.queue.fair().is_consistent());
}

// The way back has to leave it where a fair thread is expected to be, or the next switch breaks.
KTEST(RunQueue, a_thread_that_was_boosted_while_running_can_go_through_a_switch) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  Thread* other = fixture.make(PriorityClass::RealTime);
  KREQUIRE(thread != nullptr && other != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  KREQUIRE(fixture.run_next(*fixture.idle()) == thread);

  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::RealTime));
  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::Normal));

  (void)fixture.queue.enqueue_locked(*other);
  KEXPECT(fixture.queue.should_switch_locked());

  KEXPECT(fixture.run_next(*thread, PickMode::Preempted) == other);
  KEXPECT(thread->on_runqueue());
  KEXPECT(thread->is_queued());
  KEXPECT(fixture.queue.fair().current() == nullptr);
  KEXPECT(fixture.queue.fair().is_consistent());

  thread->set_state(ThreadState::Blocked);
  KEXPECT(fixture.queue.remove_locked(*thread));
}

KTEST(RunQueue, a_blocked_thread_just_takes_the_new_priority) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr);

  thread->set_state(ThreadState::Blocked);
  fixture.queue.set_priority_locked(*thread, Priority::of(PriorityClass::RealTime));

  KEXPECT(thread->priority_class() == PriorityClass::RealTime);
  KEXPECT_EQ(fixture.queue.waiting(), 0u);
  KEXPECT(!thread->on_runqueue());
}

KTEST(RunQueue, a_stolen_thread_is_marked_as_being_on_its_way) {
  Fixture fixture;
  Thread* thread = fixture.make(PriorityClass::Normal);
  KREQUIRE(thread != nullptr);

  (void)fixture.queue.enqueue_locked(*thread);
  KREQUIRE(fixture.queue.try_steal(1, 0) == thread);

  KEXPECT(thread->migrating());
  KEXPECT(!thread->on_runqueue());
  KEXPECT(thread->state() == ThreadState::Ready);

  (void)fixture.queue.enqueue_locked(*thread);
  KEXPECT(!thread->migrating());
  KEXPECT(thread->on_runqueue());
}

}  // namespace kernel::testing
