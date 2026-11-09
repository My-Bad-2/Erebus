// RCU: the read side, what waits for whom, where things are reclaimed, and stress that would show
// an object freed under a reader. Retired nodes come from a poisoned cache (nodes.hpp), so an early
// free breaks what a reader is looking at instead of going unnoticed.
#include "utils/rcu/rcu.hpp"

#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../../sched/support.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "hal/timer/timer.hpp"
#include "ktest.hpp"
#include "nodes.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/hyaline.hpp"
#include "utils/rcu/rcu_ptr.hpp"
#include "utils/rcu/srcu.hpp"

namespace kernel::testing {
namespace {
namespace rcu = utils::rcu;
namespace percpu = hw::percpu;
using klib::Duration;
using rcu_nodes::intact;
using rcu_nodes::jitter;
using rcu_nodes::Node;
using rcu_nodes::Pool;
using support::wait_for;

constexpr Duration SETTLED = Duration::milliseconds(100);

[[nodiscard]] std::uint64_t reader_word() noexcept {
  return __atomic_load_n(&percpu::self()->rcu_reader, __ATOMIC_RELAXED);
}

[[nodiscard]] std::uint64_t sections_done(const std::uint64_t word) noexcept {
  return word / percpu::RCU_SECTION_DONE;
}

// A callback that says it ran, and where.
struct Mark {
  std::atomic<std::uint32_t> runs{0};
  std::atomic<bool> interrupts_on{false};
  std::atomic<bool> in_hardirq{true};
  std::atomic<bool> in_softirq{false};
  std::atomic<bool> could_sleep{true};
};

void set_mark(void* context) noexcept {
  auto* mark = static_cast<Mark*>(context);
  mark->interrupts_on.store(hw::irq::is_enabled(), std::memory_order_relaxed);
  mark->in_hardirq.store(percpu::in_hardirq(), std::memory_order_relaxed);
  mark->in_softirq.store(percpu::in_softirq(), std::memory_order_relaxed);
  mark->could_sleep.store(percpu::can_sleep(), std::memory_order_relaxed);
  mark->runs.fetch_add(1, std::memory_order_acq_rel);
}

// A reader on another CPU that stays in its section until told to leave, checking all along that
// the node it found is intact. Everything it writes to is in here, and `left` is the last of it.
struct HeldReader {
  rcu::Ptr<Node>* slot{nullptr};
  bool nest_inside{false};  ///< Opens and closes sections inside its own, over and over.
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};
  std::atomic<bool> left{false};
  std::atomic<std::uint32_t> broken{0};
  std::atomic<bool> found{false};
};

void hold_a_reader(HeldReader* reader) noexcept {
  {
    const rcu::ReadGuard rcu;
    const Node* node = reader->slot->load(rcu);
    reader->found.store(node != nullptr, std::memory_order_relaxed);
    reader->inside.store(true, std::memory_order_release);

    while (!reader->leave.load(std::memory_order_acquire)) {
      // Half the time in an inner section and half out, so that every look at the word has an even
      // chance of finding it otherwise than the batch saw it. That is a change, but not the end of
      // the outer section.
      if (reader->nest_inside) {
        {
          rcu::ReadGuard inner;
          support::spin_for(Duration::microseconds(10));
        }

        support::spin_for(Duration::microseconds(10));
      }

      if (node != nullptr && !intact(node)) {
        reader->broken.fetch_add(1, std::memory_order_relaxed);
      }

      hw::pause();
    }
  }

  reader->left.store(true, std::memory_order_release);
}

// A type with a destructor, which rcu::retire() sends through a callback.
struct Tracked {
  std::atomic<std::uint32_t>* destroyed{nullptr};
  ~Tracked() noexcept { destroyed->fetch_add(1, std::memory_order_acq_rel); }
};

[[nodiscard]] bool have_two_cpus() noexcept { return hw::smp::online_cpus() >= 2; }

// support::wait_for() for inside a read section, where giving the CPU away is not allowed.
template <typename Pred>
[[nodiscard]] bool spin_until(Pred&& done, const Duration timeout) noexcept {
  const klib::TimePoint deadline = drivers::SystemClock::now() + timeout;
  while (!done()) {
    if (drivers::SystemClock::now() > deadline) {
      return false;
    }

    hw::pause();
  }

  return true;
}
// A writer and a reader on two CPUs, in rounds that start on both at once. Each side then waits a
// random little while, so that what they do lands close together, in either order, over and over.
struct Lockstep {
  std::atomic<std::uint64_t> ready{0};     ///< The reader is done with the round before this one.
  std::atomic<std::uint64_t> started{0};   ///< The writer has started this one.
  std::atomic<std::uint64_t> finished{0};  ///< The writer is done with this one.
  std::atomic<bool> stop{false};           ///< No round after the last one started.

  void reset() noexcept {
    ready.store(0);
    started.store(0);
    finished.store(0);
    stop.store(false);
  }

  // Writer: waits until the reader is done with the previous round, so whatever it looked at can
  // be reused from here on.
  void wait_for_reader(const std::uint64_t round) const noexcept {
    while (ready.load(std::memory_order_acquire) != round) {
      hw::pause();
    }
  }

  void start(const std::uint64_t round, Rng& rng) noexcept {
    started.store(round, std::memory_order_release);
    jitter(rng);
  }

  void finish(const std::uint64_t round) noexcept {
    finished.store(round, std::memory_order_release);
  }

  void end() noexcept { stop.store(true, std::memory_order_release); }

  // Reader: false once the writer has stopped.
  [[nodiscard]] bool join(const std::uint64_t round, Rng& rng) noexcept {
    ready.store(round, std::memory_order_release);
    while (started.load(std::memory_order_acquire) != round) {
      if (stop.load(std::memory_order_acquire)) {
        return false;
      }

      hw::pause();
    }

    jitter(rng);
    return true;
  }

  void wait_until_finished(const std::uint64_t round) const noexcept {
    while (finished.load(std::memory_order_acquire) != round) {
      hw::pause();
    }
  }
};
}  // namespace

KTEST(Rcu, a_read_section_nests_and_counts_as_one) {
  const std::uint8_t preempt = percpu::preempt_count();
  KEXPECT_EQ(percpu::rcu_depth(), 0u);
  KEXPECT(percpu::can_sleep());

  {
    rcu::ReadGuard outer;
    KEXPECT_EQ(percpu::rcu_depth(), 1u);
    KEXPECT(percpu::in_rcu_read());
    KEXPECT(!percpu::can_sleep());
    KEXPECT_EQ(percpu::preempt_count(), preempt + 1u);

    {
      rcu::ReadGuard inner;
      KEXPECT_EQ(percpu::rcu_depth(), 2u);
    }

    KEXPECT_EQ(percpu::rcu_depth(), 1u);
  }

  KEXPECT_EQ(percpu::rcu_depth(), 0u);
  KEXPECT_EQ(percpu::preempt_count(), preempt);
  KEXPECT(percpu::can_sleep());

  // Counted with interrupts off, as each interrupt is a read section too, and would count.
  std::uint64_t before = 0;
  std::uint64_t nested = 0;
  std::uint64_t after = 0;
  {
    utils::IrqDisableGuard irq_guard;
    before = reader_word();
    {
      rcu::ReadGuard outer;
      {
        rcu::ReadGuard inner;
      }

      nested = reader_word();
    }

    after = reader_word();
  }

  // Leaving a nested section finishes nothing; leaving the outermost one finishes one.
  KEXPECT_EQ(sections_done(nested), sections_done(before));
  KEXPECT_EQ(sections_done(after), sections_done(before) + 1);
}

KTEST(Rcu, deep_nesting_unwinds_exactly) {
  constexpr std::uint32_t DEPTH = 200;

  struct Recurse {
    static void into(const std::uint32_t left) noexcept {
      rcu::ReadGuard rcu;
      if (left > 1) {
        into(left - 1);
      }
    }
  };

  std::uint64_t before = 0;
  std::uint64_t after = 0;
  {
    utils::IrqDisableGuard irq_guard;
    before = reader_word();
    Recurse::into(DEPTH);
    after = reader_word();
  }

  KEXPECT_EQ(percpu::rcu_depth(), 0u);
  KEXPECT_EQ(sections_done(after), sections_done(before) + 1);
}

// A real interrupt in the middle of a section, with a section of its own: it nests, and leaves the
// word as it found it. Interrupts are dispatched in a section already, so its handler starts one
// deeper than the code it interrupted, and its own section goes one deeper still.
KTEST(Rcu, an_interrupt_inside_a_section_nests_in_it) {
  struct Seen {
    std::atomic<bool> fired{false};
    std::atomic<std::uint32_t> depth_on_entry{0};
    std::atomic<std::uint32_t> depth{0};
    std::atomic<std::uint64_t> sections{0};
  } seen;

  hw::timer::Timer timer{[](hw::timer::Timer&, void* context) noexcept {
                           auto* s = static_cast<Seen*>(context);
                           s->depth_on_entry.store(percpu::rcu_depth(), std::memory_order_relaxed);
                           {
                             rcu::ReadGuard rcu;
                             s->depth.store(percpu::rcu_depth(), std::memory_order_relaxed);
                           }
                           s->sections.store(sections_done(reader_word()),
                                             std::memory_order_relaxed);
                           s->fired.store(true, std::memory_order_release);
                           return hw::timer::TimerAction::Done;
                         },
                         &seen};

  std::uint64_t inside = 0;
  {
    rcu::ReadGuard rcu;
    inside = sections_done(reader_word());
    (void)timer.start_after(Duration::microseconds(200));
    KEXPECT(spin_until([&] { return seen.fired.load(std::memory_order_acquire); },
                       Duration::seconds(5)));
    KEXPECT_EQ(percpu::rcu_depth(), 1u);
  }

  timer.cancel_sync();
  KEXPECT_EQ(seen.depth_on_entry.load(), 2u);
  KEXPECT_EQ(seen.depth.load(), 3u);
  KEXPECT_EQ(seen.sections.load(), inside);
}

// A reschedule that comes up while a section keeps the CPU happens the moment it ends, not at some
// later interrupt.
KTEST(Rcu, a_reschedule_held_off_by_a_section_happens_when_it_ends) {
  if (!have_two_cpus()) {
    KTEST_SKIP("takes a second CPU");
  }

  struct Scene {
    std::atomic<bool> inside{false};
    std::atomic<bool> go_on{false};
    std::atomic<bool> preempter_ran{false};
    std::atomic<bool> ran_while_inside{true};
    std::atomic<bool> ran_at_the_end{false};
    std::atomic<bool> done{false};
  } scene;

  const std::uint32_t cpu = support::last_cpu();
  KREQUIRE(support::start(
               "reader",
               +[](Scene* s) noexcept {
                 {
                   rcu::ReadGuard rcu;
                   s->inside.store(true, std::memory_order_release);
                   while (!s->go_on.load(std::memory_order_acquire)) {
                     hw::pause();
                   }

                   support::spin_for(Duration::milliseconds(2));
                   s->ran_while_inside.store(s->preempter_ran.load(std::memory_order_acquire));
                 }

                 s->ran_at_the_end.store(s->preempter_ran.load(std::memory_order_acquire));
                 s->done.store(true, std::memory_order_release);
               },
               &scene, support::pinned_to(cpu)) != nullptr);

  KEXPECT(wait_for([&] { return scene.inside.load(std::memory_order_acquire); }));
  KEXPECT(support::start(
              "preempter",
              +[](Scene* s) noexcept { s->preempter_ran.store(true, std::memory_order_release); },
              &scene, support::of_class(sched::PriorityClass::RealTime, cpu)) != nullptr);

  scene.go_on.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return scene.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return scene.preempter_ran.load(std::memory_order_acquire); }));
  KEXPECT(!scene.ran_while_inside.load());
  KEXPECT(scene.ran_at_the_end.load());
}

// Nobody reads here, nearly all the time. An interrupt is a reader for as long as it lasts, though,
// and one may be going on somewhere just as the batch is sealed, so it gets a few tries.
KTEST(Rcu, with_nobody_reading_a_flush_frees_at_once) {
  Pool pool;
  KREQUIRE(pool.create());

  bool at_once = false;
  for (std::uint32_t attempt = 0; attempt < 50 && !at_once; ++attempt) {
    const rcu::Stats before = rcu::stats();
    for (std::uint64_t i = 0; i < 5; ++i) {
      rcu::retire_raw(pool.make(i));
    }

    rcu::flush_local_batch();
    const rcu::Stats after = rcu::stats();

    KEXPECT_GE(after.objects_retired - before.objects_retired, 5u);
    at_once = after.fastpath_batches > before.fastpath_batches &&
              after.objects_reclaimed - before.objects_reclaimed >= 5;
  }

  KEXPECT(at_once);

  rcu::barrier();
  pool.destroy();
}

// The batch belongs to a CPU nobody reads on. One reader elsewhere is enough to hold it, and only
// until it leaves: what that CPU does afterwards, here nothing at all, does not matter.
KTEST(Rcu, a_reader_elsewhere_holds_what_it_found_until_it_leaves) {
  if (!have_two_cpus()) {
    KTEST_SKIP("takes a second CPU");
  }

  Pool pool;
  KREQUIRE(pool.create());

  rcu::Ptr<Node> slot{pool.make(7)};
  Mark after_it{};
  HeldReader reader{.slot = &slot};

  KREQUIRE(support::start("reader", &hold_a_reader, &reader,
                          support::pinned_to(support::last_cpu())) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  rcu::retire_raw(slot.exchange(nullptr));
  rcu::call_after_grace_period(&set_mark, &after_it);
  rcu::flush_local_batch();

  support::spin_for(SETTLED);
  KEXPECT_EQ(after_it.runs.load(), 0u);

  reader.leave.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return after_it.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));

  KEXPECT(reader.found.load());
  KEXPECT_EQ(reader.broken.load(), 0u);

  rcu::barrier();
  pool.destroy();
}

// Sections inside a section change the reader's word every time, but only the outermost one ends
// what the reader is owed: a node it found at the start stays whole through any number of them.
KTEST(Rcu, inner_sections_do_not_end_what_the_outer_one_holds) {
  if (!have_two_cpus()) {
    KTEST_SKIP("takes a second CPU");
  }

  Pool pool;
  KREQUIRE(pool.create());

  rcu::Ptr<Node> slot{pool.make(9)};
  HeldReader reader{.slot = &slot, .nest_inside = true};
  KREQUIRE(support::start("nesting reader", &hold_a_reader, &reader,
                          support::pinned_to(support::last_cpu())) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  // The node in a batch of its own, which is freed right where it is found to be done: if a poll
  // took the reader for gone too early, the reader sees it at once. The callback comes after it.
  Mark after_it{};
  rcu::retire_raw(slot.exchange(nullptr));
  rcu::flush_local_batch();
  rcu::call_after_grace_period(&set_mark, &after_it);
  rcu::flush_local_batch();

  support::spin_for(SETTLED);
  KEXPECT_EQ(after_it.runs.load(), 0u);
  KEXPECT_EQ(reader.broken.load(), 0u);

  reader.leave.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));
  KEXPECT_EQ(reader.broken.load(), 0u);
  KEXPECT(wait_for([&] { return after_it.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));

  rcu::barrier();
  pool.destroy();
}

// Nothing ever fills the batch, and nobody flushes it: the timer does.
KTEST(Rcu, a_lone_retirement_is_not_forgotten) {
  Mark mark{};
  rcu::call_after_grace_period(&set_mark, &mark);
  KEXPECT(wait_for([&] { return mark.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));
}

// The retiring CPU itself is the reader. Its own timer goes off in the meantime, and must find that
// the batch still waits. A callback could not run inside the section anyway, as DPCs wait for
// preemption, so the node goes in a batch of its own: one of nothing but frees is freed right in
// the seal when it waits for nobody, and the node is looked at again after that.
KTEST(Rcu, retiring_from_a_read_section_waits_for_that_section) {
  Pool pool;
  KREQUIRE(pool.create());

  // Nothing of this CPU's still on its way, which would hold the frees back behind it.
  rcu::barrier();

  rcu::Ptr<Node> slot{pool.make(5)};
  Mark mark{};
  bool whole = true;

  {
    rcu::ReadGuard rcu;
    const Node* node = slot.load(rcu);
    const std::uint64_t waited_before = rcu::stats().hyaline_batches;
    rcu::retire_raw(slot.exchange(nullptr));
    rcu::flush_local_batch();
    KEXPECT_GT(rcu::stats().hyaline_batches, waited_before);
    whole &= intact(node);

    rcu::call_after_grace_period(&set_mark, &mark);
    rcu::flush_local_batch();

    const klib::TimePoint end = drivers::SystemClock::now() + Duration::milliseconds(10);
    while (drivers::SystemClock::now() < end) {
      whole &= intact(node);
      hw::pause();
    }

    KEXPECT_EQ(mark.runs.load(), 0u);
  }

  KEXPECT(whole);
  KEXPECT(wait_for([&] { return mark.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));

  rcu::barrier();
  pool.destroy();
}

// Callbacks run in a deferred call: interrupts on, no sleeping, and in the order they came.
KTEST(Rcu, callbacks_run_in_a_deferred_call_in_the_order_they_came) {
  constexpr std::uint32_t CALLBACKS = 300;

  struct Order {
    std::array<std::uint32_t, CALLBACKS> seen{};
    std::atomic<std::uint32_t> count{0};
    std::atomic<std::uint32_t> wrong_context{0};
  };

  struct Ticket {
    Order* order;
    std::uint32_t number;
  };

  static Order order;
  static std::array<Ticket, CALLBACKS> tickets;
  order.count.store(0);
  order.wrong_context.store(0);

  for (std::uint32_t i = 0; i < CALLBACKS; ++i) {
    tickets[i] = Ticket{.order = &order, .number = i};
    rcu::call_after_grace_period(
        [](void* context) noexcept {
          const auto* ticket = static_cast<const Ticket*>(context);
          const bool right = hw::irq::is_enabled() && !percpu::in_hardirq() &&
                             percpu::in_softirq() && !percpu::can_sleep();
          if (!right) {
            ticket->order->wrong_context.fetch_add(1, std::memory_order_relaxed);
          }

          const std::uint32_t at = ticket->order->count.load(std::memory_order_relaxed);
          ticket->order->seen[at] = ticket->number;
          ticket->order->count.store(at + 1, std::memory_order_release);
        },
        &tickets[i]);
  }

  KEXPECT(wait_for([&] { return order.count.load(std::memory_order_acquire) == CALLBACKS; },
                   Duration::seconds(5)));
  KEXPECT_EQ(order.wrong_context.load(), 0u);

  bool in_order = true;
  for (std::uint32_t i = 0; i < CALLBACKS; ++i) {
    in_order &= order.seen[i] == i;
  }

  KEXPECT(in_order);
}

// Everything a CPU retires is reclaimed in order, frees included, even one with nothing of its own
// to wait for: barrier() and `completed` count on it. With preemption off the DPC that runs the
// callback cannot, so the free behind it has to stay where it is, through the timer's polls too.
KTEST(Rcu, a_free_does_not_overtake_a_callback_retired_before_it) {
  Pool pool;
  KREQUIRE(pool.create());
  rcu::barrier();

  Node* node = pool.make(11);
  KREQUIRE(node != nullptr);
  Mark mark{};

  percpu::preempt_disable();
  rcu::call_after_grace_period(&set_mark, &mark);
  rcu::flush_local_batch();
  rcu::retire_raw(node);
  rcu::flush_local_batch();
  support::spin_for(Duration::milliseconds(5));
  const bool whole = intact(node);
  const std::uint32_t runs = mark.runs.load(std::memory_order_acquire);
  percpu::preempt_enable();

  KEXPECT(whole);
  KEXPECT_EQ(runs, 0u);
  KEXPECT(wait_for([&] { return mark.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));

  rcu::barrier();
  pool.destroy();
}

KTEST(Rcu, retire_destroys_and_frees_after_one_grace_period) {
  if (!have_two_cpus()) {
    KTEST_SKIP("takes a second CPU");
  }

  Pool pool;
  KREQUIRE(pool.create());

  rcu::Ptr<Node> slot{pool.make(1)};
  HeldReader reader{.slot = &slot};
  KREQUIRE(support::start("reader", &hold_a_reader, &reader,
                          support::pinned_to(support::last_cpu())) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  std::atomic<std::uint32_t> destroyed{0};
  auto* tracked = new Tracked{.destroyed = &destroyed};
  rcu::retire(tracked);
  rcu::flush_local_batch();

  support::spin_for(Duration::milliseconds(20));
  KEXPECT_EQ(destroyed.load(), 0u);

  reader.leave.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));
  rcu::barrier();
  KEXPECT_EQ(destroyed.load(), 1u);

  rcu::retire(static_cast<Tracked*>(nullptr));
  rcu::retire_raw(slot.exchange(nullptr));
  rcu::barrier();
  pool.destroy();
}

// From a timer callback, which is an interrupt handler: retiring works there too.
KTEST(Rcu, interrupt_handlers_can_retire) {
  Pool pool;
  KREQUIRE(pool.create());

  struct Scene {
    Pool* pool;
    Mark mark{};
    std::atomic<bool> fired{false};
  } scene{.pool = &pool};

  hw::timer::Timer timer{[](hw::timer::Timer&, void* context) noexcept {
                           auto* s = static_cast<Scene*>(context);
                           rcu::retire_raw(s->pool->make(3));
                           rcu::call_after_grace_period(&set_mark, &s->mark);
                           s->fired.store(true, std::memory_order_release);
                           return hw::timer::TimerAction::Done;
                         },
                         &scene};

  (void)timer.start_after(Duration::microseconds(100));
  KEXPECT(wait_for([&] { return scene.fired.load(std::memory_order_acquire); }));
  timer.cancel_sync();

  KEXPECT(wait_for([&] { return scene.mark.runs.load(std::memory_order_acquire) == 1; },
                   Duration::seconds(2)));
  rcu::barrier();
  pool.destroy();
}

// An interrupt is a reader while it lasts, so a batch sealed in one always waits for its own CPU.
// Not for the next poll, though: the DPC looks again as soon as the interrupt is over, which is
// before the code it interrupted goes on. A reader elsewhere can hold a batch up now and then, so
// it gets a few tries.
KTEST(Rcu, a_seal_in_an_interrupt_is_done_as_soon_as_the_interrupt_is) {
  struct Scene {
    Mark mark{};
    std::atomic<bool> fired{false};
  };

  bool at_once = false;
  for (std::uint32_t attempt = 0; attempt < 10 && !at_once; ++attempt) {
    Scene scene;
    hw::timer::Timer timer{[](hw::timer::Timer&, void* context) noexcept {
                             auto* s = static_cast<Scene*>(context);
                             rcu::call_after_grace_period(&set_mark, &s->mark);
                             rcu::flush_local_batch();
                             s->fired.store(true, std::memory_order_release);
                             return hw::timer::TimerAction::Done;
                           },
                           &scene};

    (void)timer.start_after(Duration::microseconds(100));
    KEXPECT(spin_until([&] { return scene.fired.load(std::memory_order_acquire); },
                       Duration::seconds(2)));
    at_once = scene.mark.runs.load(std::memory_order_acquire) == 1;

    timer.cancel_sync();
    KEXPECT(wait_for([&] { return scene.mark.runs.load(std::memory_order_acquire) == 1; },
                     Duration::seconds(2)));
  }

  KEXPECT(at_once);
}

KTEST(Rcu, synchronize_waits_for_every_reader_that_was_there) {
  if (!have_two_cpus()) {
    KTEST_SKIP("takes a second CPU");
  }

  rcu::Ptr<Node> slot{};
  HeldReader reader{.slot = &slot};
  KREQUIRE(support::start("reader", &hold_a_reader, &reader,
                          support::pinned_to(support::last_cpu())) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  // Lets the reader go a while from now, on a CPU of its own, while this one is in synchronize().
  hw::timer::Timer release{[](hw::timer::Timer&, void* context) noexcept {
                             static_cast<HeldReader*>(context)->leave.store(
                                 true, std::memory_order_release);
                             return hw::timer::TimerAction::Done;
                           },
                           &reader};
  (void)release.start_after(Duration::milliseconds(30));

  const std::uint64_t before = rcu::stats().synchronizes;
  rcu::synchronize();

  // `left` goes up after the section is over, so it may lag a little; `leave` may not.
  KEXPECT(reader.leave.load(std::memory_order_acquire));
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));
  KEXPECT_GT(rcu::stats().synchronizes, before);
  release.cancel_sync();
}

KTEST(Rcu, synchronize_with_nobody_reading_is_quick) {
  const klib::TimePoint start = drivers::SystemClock::now();
  for (int i = 0; i < 100; ++i) {
    rcu::synchronize();
  }

  KEXPECT(drivers::SystemClock::now() - start < Duration::milliseconds(100));
}

// Everyone retires, everyone waits for everyone, and then nothing at all is left anywhere.
KTEST_ALL_CPUS(Rcu, barrier_leaves_nothing_behind_on_any_cpu, cpu) {
  constexpr std::uint32_t PER_CPU = 500;
  static std::atomic<std::uint32_t> called{0};
  static Pool pool;

  if (cpu == 0) {
    called.store(0);
    KEXPECT(pool.create());
  }

  sync_cpus();

  // The first callback takes its time, so that what comes after it, frees that could go at once,
  // is done long before it. barrier() has to wait for it all the same.
  for (std::uint32_t i = 0; i < PER_CPU; ++i) {
    if (i == 0) {
      rcu::call_after_grace_period(
          [](void*) noexcept {
            support::spin_for(Duration::milliseconds(2));
            called.fetch_add(1, std::memory_order_relaxed);
          },
          nullptr);
    } else if (i % 3 == 0) {
      rcu::call_after_grace_period(
          [](void*) noexcept { called.fetch_add(1, std::memory_order_relaxed); }, nullptr);
    } else {
      rcu::retire_raw(pool.make(i));
    }
  }

  sync_cpus();

  if (cpu == 0) {
    rcu::barrier();
    KEXPECT_EQ(called.load(), cpu_count() * ((PER_CPU + 2) / 3));

    const rcu::Stats stats = rcu::stats();
    KEXPECT_EQ(stats.objects_retired, stats.objects_reclaimed);
    pool.destroy();
  }

  sync_cpus();
}

// More readers at once than a batch can note down without an allocation.
KTEST_ALL_CPUS(Rcu, more_readers_than_fit_in_a_batch, cpu) {
  if (cpu_count() <= rcu::detail::RetireBatch::INLINE_WAITERS + 1) {
    KTEST_SKIP("takes more CPUs than a batch has room for readers");
  }

  static std::atomic<bool> retired{false};
  static Mark mark{};

  if (cpu == 0) {
    retired.store(false);
    mark.runs.store(0);
  }

  sync_cpus();

  if (cpu == 0) {
    // Everyone else is in a read section by the second sync.
    sync_cpus();
    const std::uint64_t waited = rcu::stats().hyaline_batches;
    rcu::call_after_grace_period(&set_mark, &mark);
    rcu::flush_local_batch();
    KEXPECT_GT(rcu::stats().hyaline_batches, waited);
    retired.store(true, std::memory_order_release);

    support::spin_for(Duration::milliseconds(10));
    KEXPECT_EQ(mark.runs.load(), 0u);
    sync_cpus();
  } else {
    rcu::ReadGuard rcu;
    sync_cpus();
    while (!retired.load(std::memory_order_acquire)) {
      hw::pause();
    }

    // Spinning in a read section is fine; sleeping is what is not.
    sync_cpus();
  }

  if (cpu == 0) {
    KEXPECT(wait_for([&] { return mark.runs.load(std::memory_order_acquire) == 1; },
                     Duration::seconds(2)));
  }

  sync_cpus();
}

// The two halves of the protocol on their own. A reader marks itself and then loads; a writer
// stores, fences, and then looks at the mark. Either the writer sees the mark or the reader sees
// the store, never neither: that outcome is the store-buffer reordering x86 allows, and the locked
// increment on entry is what rules it out.
KTEST_ALL_CPUS(Rcu, a_reader_and_a_writer_never_miss_each_other, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(300);
  constexpr std::uint32_t READER = 1;
  static Lockstep step;
  static std::atomic<std::uint64_t> flag{0};
  static std::atomic<bool> writer_saw_reader{false};
  static std::atomic<std::uint64_t> missed{0};
  static std::atomic<std::uint64_t> reader_first{0};
  static std::atomic<std::uint64_t> rounds{0};

  if (cpu == 0) {
    step.reset();
    flag.store(0);
    missed.store(0);
    reader_first.store(0);
    rounds.store(0);
  }

  sync_cpus();

  Rng rng{0x5B + cpu};
  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    for (std::uint64_t round = 1; drivers::SystemClock::now() < end; ++round) {
      step.wait_for_reader(round);
      step.start(round, rng);

      // What find_readers() does, and nothing else.
      flag.store(round, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      const std::uint64_t word =
          __atomic_load_n(&percpu::cpu_area(READER)->rcu_reader, __ATOMIC_ACQUIRE);

      writer_saw_reader.store((word & percpu::RCU_DEPTH_MASK) != 0, std::memory_order_relaxed);
      step.finish(round);
    }

    step.end();
  } else if (cpu == READER) {
    for (std::uint64_t round = 1; step.join(round, rng); ++round) {
      rcu::ReadGuard rcu;
      const bool saw_store = flag.load(std::memory_order_relaxed) == round;

      // In the section until the writer has looked, so what it saw was this section or nothing.
      step.wait_until_finished(round);
      if (!saw_store) {
        reader_first.fetch_add(1, std::memory_order_relaxed);
        missed.fetch_add(writer_saw_reader.load(std::memory_order_relaxed) ? 0 : 1,
                         std::memory_order_relaxed);
      }

      rounds.fetch_add(1, std::memory_order_relaxed);
    }
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(missed.load(), 0u);
    KEXPECT_GT(rounds.load(), 1000u);
    KEXPECT_GT(reader_first.load(), 0u);
    utils::logger::info("Rcu: {} rounds, the reader first in {}\n", rounds.load(),
                        reader_first.load());
  }

  sync_cpus();
}

// The same race through the real thing. The writer unlinks with Ptr::store(), which on x86 is a
// plain store that can sit in the store buffer for a while, then retires and seals at once. Nothing
// locked comes in between, so the fence in the seal is all that keeps its look at the readers from
// going ahead of the store. Nobody else is reading, so a batch that missed the reader is freed
// right there, and the reader is left looking at poison.
KTEST_ALL_CPUS(Rcu, a_node_unlinked_with_a_plain_store_is_not_freed_under_a_reader, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(300);
  constexpr std::uint32_t READER = 1;
  static Lockstep step;
  static rcu::Ptr<Node> slot;
  static Pool pool;
  static std::atomic<std::uint64_t> broken{0};
  static std::atomic<std::uint64_t> found_old{0};
  static std::atomic<std::uint64_t> rounds{0};

  if (cpu == 0) {
    step.reset();
    broken.store(0);
    found_old.store(0);
    rounds.store(0);
    KEXPECT(pool.create());
    slot.store(pool.make(0));
  }

  sync_cpus();

  Rng rng{0xA7 + cpu};
  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    for (std::uint64_t round = 1; drivers::SystemClock::now() < end; ++round) {
      // Only now: the node the reader looked at last round may be the one this gets back.
      step.wait_for_reader(round);
      Node* fresh = pool.make(round);
      if (fresh == nullptr) {
        break;
      }

      step.start(round, rng);
      Node* old = slot.load_writer_locked();
      slot.store(fresh);
      rcu::retire_raw(old);
      rcu::flush_local_batch();
      step.finish(round);
    }

    step.end();
  } else if (cpu == READER) {
    for (std::uint64_t round = 1; step.join(round, rng); ++round) {
      rcu::ReadGuard rcu;
      const Node* node = slot.load(rcu);

      // Once the writer is through, the node is either still there or the writer missed us.
      step.wait_until_finished(round);
      if (node != nullptr) {
        broken.fetch_add(intact(node) ? 0 : 1, std::memory_order_relaxed);
        found_old.fetch_add(__atomic_load_n(&node->value, __ATOMIC_RELAXED) != round ? 1 : 0,
                            std::memory_order_relaxed);
      }

      rounds.fetch_add(1, std::memory_order_relaxed);
    }
  }

  sync_cpus();

  if (cpu == 0) {
    rcu::retire_raw(slot.exchange(nullptr));
    rcu::barrier();
    KEXPECT_EQ(broken.load(), 0u);
    KEXPECT_GT(rounds.load(), 1000u);
    KEXPECT_GT(found_old.load(), 0u);
    utils::logger::info("Rcu: {} rounds, the old node found in {}\n", rounds.load(),
                        found_old.load());
    pool.destroy();
  }

  sync_cpus();
}

// The same race again, free running and on every pair of CPUs at once: a writer unlinks and seals,
// over and over, while a reader on the next CPU loads and looks, as tight as it gets. Readers of
// the other pairs keep the batches waiting now and then, so this goes through the polls as well.
KTEST_ALL_CPUS(Rcu, a_freshly_unlinked_node_is_never_freed_under_a_reader, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(300);
  static constexpr std::uint32_t MAX_PAIRS = 32;
  static std::array<rcu::Ptr<Node>, MAX_PAIRS> slots;
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> broken{0};
  static std::atomic<std::uint64_t> reads{0};
  static std::atomic<std::uint64_t> writes{0};
  static Pool pool;

  const std::uint32_t pairs = std::min(cpu_count() / 2, MAX_PAIRS);
  const std::uint32_t pair = cpu / 2;
  const bool writer = cpu % 2 == 0;

  if (cpu == 0) {
    stop.store(false);
    broken.store(0);
    reads.store(0);
    writes.store(0);
    KEXPECT(pool.create());
    for (std::uint32_t i = 0; i < pairs; ++i) {
      slots[i].store(pool.make(i));
    }
  }

  sync_cpus();

  if (pair < pairs && writer) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t value = 0;
    while (drivers::SystemClock::now() < end) {
      if (Node* fresh = pool.make(++value)) {
        rcu::retire_raw(slots[pair].exchange(fresh));
        rcu::flush_local_batch();
      }
    }

    writes.fetch_add(value, std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
  } else if (pair < pairs) {
    std::uint64_t local_reads = 0;
    std::uint64_t local_broken = 0;
    while (!stop.load(std::memory_order_acquire)) {
      rcu::ReadGuard rcu;
      if (const Node* node = slots[pair].load(rcu)) {
        local_broken += intact(node) ? 0 : 1;
        ++local_reads;
      }
    }

    reads.fetch_add(local_reads, std::memory_order_relaxed);
    broken.fetch_add(local_broken, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    for (std::uint32_t i = 0; i < pairs; ++i) {
      rcu::retire_raw(slots[i].exchange(nullptr));
    }

    rcu::barrier();
    KEXPECT_EQ(broken.load(), 0u);
    KEXPECT_GT(reads.load(), 0u);
    KEXPECT_GT(writes.load(), 0u);
    pool.destroy();
  }

  sync_cpus();
}

// Everything at once: writers replacing nodes in shared slots, retiring from read sections and
// outside them, frees and callbacks, flushes now and then, while readers on every other CPU nest
// sections at random and look at what they found more than once, with pauses in between.
KTEST_ALL_CPUS(Rcu, readers_never_see_freed_memory_whatever_writers_do, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(400);
  static constexpr std::uint32_t SLOTS = 8;
  static std::array<rcu::Ptr<Node>, SLOTS> slots;
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> broken{0};
  static std::atomic<std::uint64_t> reads{0};
  static std::atomic<std::uint64_t> callbacks_ran{0};
  static std::atomic<std::uint64_t> callbacks_queued{0};
  static Pool pool;

  // A quarter of the CPUs write, at least one; the rest read.
  const std::uint32_t writers = std::max(cpu_count() / 4, 1u);
  const bool writer = cpu < writers;

  if (cpu == 0) {
    stop.store(false);
    broken.store(0);
    reads.store(0);
    callbacks_ran.store(0);
    callbacks_queued.store(0);
    KEXPECT(pool.create());
    for (std::uint32_t i = 0; i < SLOTS; ++i) {
      slots[i].store(pool.make(i));
    }
  }

  sync_cpus();

  Rng rng{0xC0DE + cpu};
  if (writer) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t value = std::uint64_t{cpu} << 48;
    while (drivers::SystemClock::now() < end) {
      Node* fresh = pool.make(++value);
      if (fresh == nullptr) {
        continue;
      }

      const std::uint64_t dice = rng.next();
      auto& slot = slots[dice % SLOTS];

      const auto replace = [&] {
        Node* old = slot.exchange(fresh);
        if ((dice >> 8) % 8 == 0) {
          // A callback that still finds the node whole, frees it itself, and counts.
          callbacks_queued.fetch_add(1, std::memory_order_relaxed);
          rcu::call_after_grace_period(
              [](void* node) noexcept {
                if (!intact(static_cast<Node*>(node))) {
                  broken.fetch_add(1, std::memory_order_relaxed);
                }

                memory::heap::kfree(node);
                callbacks_ran.fetch_add(1, std::memory_order_relaxed);
              },
              old);
        } else {
          rcu::retire_raw(old);
        }
      };

      if ((dice >> 16) % 4 == 0) {
        rcu::ReadGuard rcu;
        replace();
      } else {
        replace();
      }

      if ((dice >> 24) % 16 == 0) {
        rcu::flush_local_batch();
      }
    }

    if (cpu == 0) {
      stop.store(true, std::memory_order_release);
    }
  } else {
    std::uint64_t local_reads = 0;
    std::uint64_t local_broken = 0;

    while (!stop.load(std::memory_order_acquire)) {
      const std::uint64_t dice = rng.next();
      const auto look = [&] {
        rcu::ReadGuard rcu;
        if (const Node* node = slots[dice % SLOTS].load(rcu)) {
          local_broken += intact(node) ? 0 : 1;
          for (std::uint64_t spin = (dice >> 8) % 256; spin != 0; --spin) {
            hw::pause();
          }

          local_broken += intact(node) ? 0 : 1;
          ++local_reads;
        }
      };

      switch ((dice >> 16) % 3) {
        case 0:
          look();
          break;
        case 1: {
          rcu::ReadGuard outer;
          look();
          break;
        }
        default: {
          rcu::ReadGuard outer;
          rcu::ReadGuard middle;
          look();
          break;
        }
      }
    }

    reads.fetch_add(local_reads, std::memory_order_relaxed);
    broken.fetch_add(local_broken, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    for (auto& slot : slots) {
      rcu::retire_raw(slot.exchange(nullptr));
    }

    rcu::barrier();
    KEXPECT_EQ(broken.load(), 0u);
    KEXPECT_GT(reads.load(), 0u);
    KEXPECT_EQ(callbacks_ran.load(), callbacks_queued.load());

    const rcu::Stats stats = rcu::stats();
    KEXPECT_EQ(stats.objects_retired, stats.objects_reclaimed);
    pool.destroy();
  }

  sync_cpus();
}

KTEST(RcuPtr, publishes_swaps_and_compares) {
  Node first{.value = 1, .check = ~std::uint64_t{1}};
  Node second{.value = 2, .check = ~std::uint64_t{2}};

  rcu::Ptr<Node> ptr{};
  {
    rcu::ReadGuard rcu;
    KEXPECT(ptr.load(rcu) == nullptr);
  }

  ptr.store(&first);
  KEXPECT(ptr.load_writer_locked() == &first);

  Node* expected = &second;
  KEXPECT(!ptr.compare_exchange_strong(expected, &second));
  KEXPECT(expected == &first);
  KEXPECT(ptr.compare_exchange_strong(expected, &second));

  KEXPECT(ptr.exchange(nullptr) == &second);
  {
    rcu::ReadGuard rcu;
    KEXPECT(ptr.load(rcu) == nullptr);
  }

  static_assert(rcu::ReadLockWitness<rcu::ReadGuard>);
  static_assert(rcu::ReadLockWitness<const rcu::ReadGuard&>);
  static_assert(rcu::ReadLockWitness<rcu::SleepableReadGuard>);
  static_assert(!rcu::ReadLockWitness<int>);
}
}  // namespace kernel::testing
