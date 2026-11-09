// Sleepable RCU: readers that sleep and wander between CPUs, writers that wait for exactly them,
// and a stress run that would show a node freed under a reader that was asleep in its section.
#include "utils/rcu/srcu.hpp"

#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../../sched/support.hpp"
#include "drivers/clock.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "memory/heap.hpp"
#include "nodes.hpp"
#include "sched/sleep.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/rcu_ptr.hpp"

namespace kernel::testing {
namespace {
namespace rcu = utils::rcu;
using klib::Duration;
using rcu_nodes::intact;
using rcu_nodes::jitter;
using rcu_nodes::Node;
using rcu_nodes::Pool;
using support::wait_for;

// A reader that sleeps in its section until told to leave, so that it may well wake up elsewhere.
struct SleepyReader {
  rcu::SleepableDomain* domain{nullptr};
  rcu::Ptr<Node>* slot{nullptr};
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};
  std::atomic<bool> left{false};
  std::atomic<std::uint32_t> broken{0};
  std::atomic<std::uint32_t> cpus_seen{0};  // a bit per CPU, of the first 32
};

void sleep_in_a_section(SleepyReader* reader) noexcept {
  {
    rcu::SleepableReadGuard guard{*reader->domain};
    const Node* node = reader->slot->load(guard);
    reader->inside.store(true, std::memory_order_release);

    while (!reader->leave.load(std::memory_order_acquire)) {
      (void)sched::sleep_for(Duration::microseconds(500));
      reader->cpus_seen.fetch_or(1u << (hw::percpu::id() % 32), std::memory_order_relaxed);
      if (node != nullptr && !intact(node)) {
        reader->broken.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  reader->left.store(true, std::memory_order_release);
}
}  // namespace

KTEST(SleepableRcu, a_section_may_sleep_and_counts_as_a_reader_until_it_ends) {
  constinit static rcu::SleepableDomain domain{};
  KREQUIRE(domain.initialize());

  std::uint32_t token = 0;
  {
    rcu::SleepableReadGuard guard{domain};
    KEXPECT(hw::percpu::can_sleep());
    KEXPECT(sched::sleep_for(Duration::microseconds(100)));

    // Whatever generation it counted itself in is the one with a reader now.
    token = domain.read_lock();
    KEXPECT(domain.has_readers(token));
    domain.read_unlock(token);
  }

  KEXPECT(!domain.has_readers(0));
  KEXPECT(!domain.has_readers(1));
  domain.destroy();
}

// Before initialize() the domain counts in shared counters; a reader that started then and ends
// after it still has to be waited for.
KTEST(SleepableRcu, works_before_initialize_and_across_it) {
  constinit static rcu::SleepableDomain domain{};

  domain.synchronize();
  const std::uint32_t early = domain.read_lock();
  KEXPECT(domain.has_readers(early));

  KREQUIRE(domain.initialize());
  KEXPECT(domain.has_readers(early));

  domain.read_unlock(early);
  KEXPECT(!domain.has_readers(early));
  domain.synchronize();
  domain.destroy();
}

// The sums in has_readers() race with readers that come and go while they are taken, and must
// still never miss one that is in for the whole time. Unlocks first, then locks, is what makes
// that so: the other way round, a short reader that both started and ended between the two looks
// at its CPU is counted out without having been counted in, and cancels the one that is there.
KTEST_ALL_CPUS(SleepableRcu, the_sums_never_miss_a_reader_however_many_come_and_go, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("takes two CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(200);
  constinit static rcu::SleepableDomain domain{};
  static std::atomic<std::uint32_t> held{0};
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> missed{0};
  static std::atomic<std::uint64_t> looks{0};
  static std::atomic<std::uint64_t> sections{0};

  if (cpu == 0) {
    KEXPECT(domain.initialize());
    stop.store(false);
    missed.store(0);
    looks.store(0);
    sections.store(0);
  }

  sync_cpus();

  // One reader in for the whole run, on CPU 1, which comes and goes like the rest besides.
  if (cpu == 1) {
    held.store(domain.read_lock(), std::memory_order_release);
  }

  sync_cpus();

  Rng rng{0xC0FE + cpu};
  if (cpu == 0) {
    const std::uint32_t generation = held.load(std::memory_order_acquire);
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t local_looks = 0;
    std::uint64_t local_missed = 0;
    while (drivers::SystemClock::now() < end) {
      local_missed += domain.has_readers(generation) ? 0 : 1;
      ++local_looks;
    }

    looks.store(local_looks, std::memory_order_relaxed);
    missed.store(local_missed, std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
  } else {
    // About as long between sections as a look over every CPU's counters takes, so that a look
    // often has one or two of them start and end in the middle of it.
    std::uint64_t local_sections = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const std::uint32_t token = domain.read_lock();
      jitter(rng, 64);
      domain.read_unlock(token);
      jitter(rng, 1024);
      ++local_sections;
    }

    sections.fetch_add(local_sections, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 1) {
    domain.read_unlock(held.load(std::memory_order_relaxed));
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(missed.load(), 0u);
    KEXPECT_GT(looks.load(), 1000u);
    KEXPECT_GT(sections.load(), 1000u);
    KEXPECT(!domain.has_readers(0));
    KEXPECT(!domain.has_readers(1));
    domain.destroy();
  }

  sync_cpus();
}

// read_lock() takes two steps, reading the generation and then counting itself in it, and an
// interrupt can come in between and last as long as it likes. A reader held up there while a flip
// goes by counts itself in the generation that is no longer current, which the next synchronize()
// must wait for all the same: that is what its first wait is for.
//
// IPIs that spin for a while keep landing on a CPU that does nothing but enter sections, over and
// over, while another CPU replaces, waits and frees. Now and then one comes in between the two
// steps. A reader that was interrupted inside read_lock() stays in for a while afterwards, looking
// at what it found, which a writer that did not wait for it would have freed by then.
KTEST_ALL_CPUS(SleepableRcu, a_reader_held_up_inside_read_lock_is_still_waited_for, cpu) {
  if (cpu_count() < 3) {
    KTEST_SKIP("takes three CPUs");
  }

  constexpr Duration RUN = Duration::milliseconds(300);
  constexpr std::uint32_t SENDER = 0;
  constexpr std::uint32_t READER = 1;
  constexpr std::uint32_t WRITER = 2;
  constinit static rcu::SleepableDomain domain{};
  static rcu::Ptr<Node> slot;
  static Pool pool;
  static std::atomic<bool> stop{false};
  static std::atomic<bool> in_read_lock{false};  // the reader's own, and its interrupts'
  static std::atomic<bool> interrupted{false};
  static std::atomic<std::uint64_t> broken{0};
  static std::atomic<std::uint64_t> held_up{0};
  static std::atomic<std::uint64_t> ipis{0};
  static std::atomic<std::uint64_t> grace_periods{0};

  if (cpu == SENDER) {
    KEXPECT(domain.initialize());
    KEXPECT(pool.create());
    slot.store(pool.make(0));
    stop.store(false);
    in_read_lock.store(false);
    interrupted.store(false);
    broken.store(0);
    held_up.store(0);
    ipis.store(0);
    grace_periods.store(0);
  }

  sync_cpus();

  // On the reader's CPU, with interrupts off, for long enough that some flips go by.
  constexpr hw::ipi::Function HOLD_UP = [](void*) {
    if (in_read_lock.load(std::memory_order_relaxed)) {
      interrupted.store(true, std::memory_order_relaxed);
    }

    static std::uint64_t calls = 0;
    Rng rng{++calls};
    support::spin_for(Duration::microseconds(20 + static_cast<std::int64_t>(rng.next() % 200)));
  };

  if (cpu == SENDER) {
    Rng rng{0x1D};
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t sent = 0;
    while (drivers::SystemClock::now() < end) {
      sent += hw::ipi::call(READER, HOLD_UP, nullptr, hw::ipi::Wait::No) ? 1 : 0;
      support::spin_for(Duration::microseconds(250 + static_cast<std::int64_t>(rng.next() % 250)));
    }

    ipis.store(sent, std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
  } else if (cpu == READER) {
    // Read with nothing but the token: the guard would do the two steps out of sight.
    struct Inside {
      static constexpr void is_rcu_read_guard_witness() noexcept {}
    };

    std::uint64_t local_broken = 0;
    std::uint64_t local_held_up = 0;
    while (!stop.load(std::memory_order_acquire)) {
      in_read_lock.store(true, std::memory_order_relaxed);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const std::uint32_t token = domain.read_lock();
      std::atomic_signal_fence(std::memory_order_seq_cst);
      in_read_lock.store(false, std::memory_order_relaxed);

      const Node* node = slot.load(Inside{});
      if (interrupted.load(std::memory_order_relaxed)) {
        interrupted.store(false, std::memory_order_relaxed);
        ++local_held_up;

        const klib::TimePoint until = drivers::SystemClock::now() + Duration::microseconds(200);
        while (drivers::SystemClock::now() < until) {
          local_broken += node != nullptr && !intact(node) ? 1 : 0;
          hw::pause();
        }
      } else {
        local_broken += node != nullptr && !intact(node) ? 1 : 0;
      }

      domain.read_unlock(token);
    }

    broken.store(local_broken, std::memory_order_relaxed);
    held_up.store(local_held_up, std::memory_order_relaxed);
  } else if (cpu == WRITER) {
    std::uint64_t value = 0;
    std::uint64_t local_grace_periods = 0;
    while (!stop.load(std::memory_order_acquire)) {
      Node* fresh = pool.make(++value);
      if (fresh == nullptr) {
        continue;
      }

      Node* old = slot.exchange(fresh);
      domain.synchronize();
      memory::heap::kfree(old);
      ++local_grace_periods;
    }

    grace_periods.store(local_grace_periods, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == SENDER) {
    domain.synchronize();
    memory::heap::kfree(slot.exchange(nullptr));

    KEXPECT_EQ(broken.load(), 0u);
    KEXPECT_GT(held_up.load(), 0u);
    KEXPECT_GT(grace_periods.load(), 0u);
    utils::logger::info(
        "SleepableRcu: {} IPIs, {} readers held up in read_lock(), {} grace periods\n", ipis.load(),
        held_up.load(), grace_periods.load());

    pool.destroy();
    domain.destroy();
  }

  sync_cpus();
}

KTEST(SleepableRcu, synchronize_waits_for_a_reader_asleep_in_its_section) {
  constinit static rcu::SleepableDomain domain{};
  KREQUIRE(domain.initialize());

  rcu::Ptr<Node> slot{};
  SleepyReader reader{.domain = &domain, .slot = &slot};
  KREQUIRE(support::start("sleepy", &sleep_in_a_section, &reader) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  struct Release {
    SleepyReader* reader;
    std::atomic<bool> done{false};
  } release{.reader = &reader};

  KREQUIRE(support::start(
               "release",
               +[](Release* r) noexcept {
                 (void)sched::sleep_for(Duration::milliseconds(30));
                 r->reader->leave.store(true, std::memory_order_release);
                 r->done.store(true, std::memory_order_release);
               },
               &release) != nullptr);

  domain.synchronize();

  // The reader can only have left once told to. `left` comes a moment after the section ends, and
  // is the reader's last word in this frame, so it is waited for either way.
  KEXPECT(reader.leave.load(std::memory_order_acquire));
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return release.done.load(std::memory_order_acquire); }));
  domain.destroy();
}

KTEST(SleepableRcu, domains_do_not_wait_for_each_other) {
  constinit static rcu::SleepableDomain busy{};
  constinit static rcu::SleepableDomain idle{};
  KREQUIRE(busy.initialize());
  KREQUIRE(idle.initialize());

  rcu::Ptr<Node> slot{};
  SleepyReader reader{.domain = &busy, .slot = &slot};
  KREQUIRE(support::start("sleepy", &sleep_in_a_section, &reader) != nullptr);
  KEXPECT(wait_for([&] { return reader.inside.load(std::memory_order_acquire); }));

  const klib::TimePoint start = drivers::SystemClock::now();
  idle.synchronize();
  KEXPECT(drivers::SystemClock::now() - start < Duration::milliseconds(50));
  KEXPECT(!reader.left.load(std::memory_order_acquire));

  reader.leave.store(true, std::memory_order_release);
  busy.synchronize();
  KEXPECT(wait_for([&] { return reader.left.load(std::memory_order_acquire); }));

  busy.destroy();
  idle.destroy();
}

// Writers replace, wait and free by hand, over and over, while readers sleep in their sections for
// a random while and move between CPUs as they wake. A writer that stopped waiting too early frees
// a node some reader is asleep on, and that reader finds it poisoned.
KTEST(SleepableRcu, sleeping_readers_never_see_freed_memory) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes a second CPU");
  }

  constexpr Duration RUN = Duration::milliseconds(400);
  static constexpr std::uint32_t READERS = 6;
  static constexpr std::uint32_t WRITERS = 2;
  static constexpr std::uint32_t SLOTS = 4;

  struct Shared {
    rcu::SleepableDomain domain{};
    std::array<rcu::Ptr<Node>, SLOTS> slots{};
    Pool pool{};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> broken{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint32_t> finished{0};
    std::atomic<std::uint32_t> cpus_seen{0};
  };

  struct Role {
    Shared* shared;
    std::uint32_t number;
  };

  static Shared shared;
  static std::array<Role, READERS + WRITERS> roles;

  shared.stop.store(false);
  shared.broken.store(0);
  shared.reads.store(0);
  shared.frees.store(0);
  shared.finished.store(0);
  shared.cpus_seen.store(0);
  KREQUIRE(shared.domain.initialize());
  KREQUIRE(shared.pool.create());
  for (std::uint32_t i = 0; i < SLOTS; ++i) {
    shared.slots[i].store(shared.pool.make(i));
  }

  for (std::uint32_t i = 0; i < READERS + WRITERS; ++i) {
    roles[i] = Role{.shared = &shared, .number = i};
  }

  for (std::uint32_t i = 0; i < READERS; ++i) {
    KEXPECT(
        support::start(
            "reader",
            +[](Role* role) noexcept {
              Shared& s = *role->shared;
              Rng rng{0x5EED + role->number};
              while (!s.stop.load(std::memory_order_acquire)) {
                const std::uint64_t dice = rng.next();
                rcu::SleepableReadGuard guard{s.domain};
                if (const Node* node = s.slots[dice % SLOTS].load(guard)) {
                  s.broken.fetch_add(intact(node) ? 0 : 1, std::memory_order_relaxed);
                  (void)sched::sleep_for(
                      Duration::microseconds(20 + static_cast<std::int64_t>((dice >> 8) % 400)));
                  s.cpus_seen.fetch_or(1u << (hw::percpu::id() % 32), std::memory_order_relaxed);
                  s.broken.fetch_add(intact(node) ? 0 : 1, std::memory_order_relaxed);
                  s.reads.fetch_add(1, std::memory_order_relaxed);
                }
              }

              s.finished.fetch_add(1, std::memory_order_release);
            },
            &roles[i]) != nullptr);
  }

  for (std::uint32_t i = READERS; i < READERS + WRITERS; ++i) {
    KEXPECT(support::start(
                "writer",
                +[](Role* role) noexcept {
                  Shared& s = *role->shared;
                  Rng rng{0xF00D + role->number};
                  std::uint64_t value = std::uint64_t{role->number} << 48;
                  while (!s.stop.load(std::memory_order_acquire)) {
                    const std::uint32_t at = static_cast<std::uint32_t>(rng.next() % SLOTS);
                    Node* fresh = s.pool.make(++value);
                    if (fresh == nullptr) {
                      continue;
                    }

                    // Writers race for slots too: each old node comes out of exactly one exchange.
                    Node* old = s.slots[at].exchange(fresh);

                    s.domain.synchronize();
                    memory::heap::kfree(old);
                    s.frees.fetch_add(1, std::memory_order_relaxed);
                  }

                  s.finished.fetch_add(1, std::memory_order_release);
                },
                &roles[i]) != nullptr);
  }

  (void)sched::sleep_for(RUN);
  shared.stop.store(true, std::memory_order_release);
  KEXPECT(wait_for(
      [&] { return shared.finished.load(std::memory_order_acquire) == READERS + WRITERS; }));

  for (auto& slot : shared.slots) {
    memory::heap::kfree(slot.exchange(nullptr));
  }

  KEXPECT_EQ(shared.broken.load(), 0u);
  KEXPECT_GT(shared.reads.load(), 0u);
  KEXPECT_GT(shared.frees.load(), 0u);
  shared.pool.destroy();
  shared.domain.destroy();
}

// Several writers synchronize at once, with readers coming and going: they take turns, and every
// one of them comes back.
KTEST(SleepableRcu, writers_that_synchronize_at_once_take_turns) {
  static constexpr std::uint32_t WRITERS = 4;
  static constexpr std::uint32_t ROUNDS = 25;

  struct Shared {
    rcu::SleepableDomain domain{};
    std::atomic<std::uint32_t> done{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> reader_done{false};
  };

  static Shared shared;
  shared.done.store(0);
  shared.stop.store(false);
  shared.reader_done.store(false);
  KREQUIRE(shared.domain.initialize());

  KREQUIRE(support::start(
               "reader",
               +[](Shared* s) noexcept {
                 while (!s->stop.load(std::memory_order_acquire)) {
                   rcu::SleepableReadGuard guard{s->domain};
                   (void)sched::sleep_for(Duration::microseconds(50));
                 }

                 s->reader_done.store(true, std::memory_order_release);
               },
               &shared) != nullptr);

  for (std::uint32_t i = 0; i < WRITERS; ++i) {
    KEXPECT(support::start(
                "writer",
                +[](Shared* s) noexcept {
                  for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                    s->domain.synchronize();
                  }

                  s->done.fetch_add(1, std::memory_order_release);
                },
                &shared) != nullptr);
  }

  KEXPECT(wait_for([&] { return shared.done.load(std::memory_order_acquire) == WRITERS; }));
  shared.stop.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return shared.reader_done.load(std::memory_order_acquire); }));
  shared.domain.destroy();
}
}  // namespace kernel::testing
