// The interrupt manager: handlers are found without a lock, in an RCU read section, and a handler
// that is taken off is waited for on every CPU before the caller gets it back.
#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "sched/sleep.hpp"
#include "utils/logger.hpp"

namespace kernel::testing {
namespace {
using hw::interrupts::Event;
using hw::interrupts::InterruptManager;
using hw::interrupts::InterruptRegistration;
using hw::interrupts::InterruptStatus;
using klib::Duration;
using support::wait_for;
namespace percpu = hw::percpu;

// Nobody else registers these; tests/kernel/sched/dpc.cpp has 0xC8.
constexpr std::uint8_t SLOW_VECTOR = 0xC9;
constexpr std::uint8_t BUSY_VECTOR = 0xCA;

void raise(const std::uint8_t vector) noexcept { percpu::lapic().send_self_ipi(vector); }

struct Looked {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<bool> in_read_section{false};
};

InterruptStatus look_around(const Event&, void* context) noexcept {
  auto* looked = static_cast<Looked*>(context);
  looked->in_read_section.store(percpu::in_rcu_read(), std::memory_order_relaxed);
  looked->calls.fetch_add(1, std::memory_order_release);
  return InterruptStatus::Handled;
}

// Stays in the handler, with interrupts off, until told to go, or for two seconds at most.
struct Held {
  std::atomic<bool> inside{false};
  std::atomic<bool> release{false};
  std::atomic<bool> out{false};
  std::atomic<bool> raised{false};
};

InterruptStatus hold_on(const Event&, void* context) noexcept {
  auto* held = static_cast<Held*>(context);
  held->inside.store(true, std::memory_order_release);

  const klib::TimePoint deadline = drivers::SystemClock::now() + Duration::seconds(2);
  while (!held->release.load(std::memory_order_acquire) && drivers::SystemClock::now() < deadline) {
    hw::pause();
  }

  held->out.store(true, std::memory_order_release);
  return InterruptStatus::Handled;
}

// Handlers that know whether they are supposed to be registered. Once unregister_handler() has
// returned, one is marked dead, and no call may find it so.
constexpr std::uint64_t LIVE = 0x4C49'5645'4C49'5645ull;
constexpr std::uint64_t DEAD = 0xDEAD'DEAD'DEAD'DEADull;
constexpr std::uint32_t CHURNED = 4;

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_calls_to_the_dead{0};
std::array<std::atomic<std::uint64_t>, CHURNED> g_states{};

InterruptStatus check_alive(const Event&, void* context) noexcept {
  const auto* state = static_cast<const std::atomic<std::uint64_t>*>(context);
  if (state->load(std::memory_order_relaxed) != LIVE) {
    g_calls_to_the_dead.fetch_add(1, std::memory_order_relaxed);
  }

  g_calls.fetch_add(1, std::memory_order_relaxed);
  return InterruptStatus::Handled;
}

constinit InterruptRegistration g_churned[CHURNED] = {
    {&check_alive, &g_states[0]},
    {&check_alive, &g_states[1]},
    {&check_alive, &g_states[2]},
    {&check_alive, &g_states[3]},
};
}  // namespace

// Which is what lets unregister_handler() wait for them.
KTEST(InterruptManager, handlers_run_in_a_read_section) {
  Looked looked;
  InterruptRegistration registration{&look_around, &looked};
  InterruptManager::register_handler(SLOW_VECTOR, &registration);

  raise(SLOW_VECTOR);
  KEXPECT(wait_for([&] { return looked.calls.load(std::memory_order_acquire) == 1; }));
  KEXPECT(looked.in_read_section.load());
  KEXPECT(!percpu::in_rcu_read());

  // Gone means gone: the next one finds nobody.
  InterruptManager::unregister_handler(SLOW_VECTOR, &registration);
  raise(SLOW_VECTOR);
  support::spin_for(Duration::milliseconds(1));
  KEXPECT_EQ(looked.calls.load(), 1u);
}

// A handler in the middle of a call on another CPU, there for as long as it takes. Taking it off
// must not come back before the call is over: the registration is the caller's again after that.
KTEST(InterruptManager, unregistering_waits_for_a_handler_still_running_elsewhere) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes a second CPU");
  }

  Held held;
  InterruptRegistration registration{&hold_on, &held};
  InterruptManager::register_handler(SLOW_VECTOR, &registration);

  KREQUIRE(support::start(
               "raise",
               +[](Held* h) noexcept {
                 raise(SLOW_VECTOR);
                 h->raised.store(true, std::memory_order_release);
               },
               &held, support::pinned_to(support::last_cpu())) != nullptr);
  KREQUIRE(wait_for([&] { return held.inside.load(std::memory_order_acquire); }));

  KREQUIRE(support::start(
               "release",
               +[](Held* h) noexcept {
                 (void)sched::sleep_for(Duration::milliseconds(20));
                 h->release.store(true, std::memory_order_release);
               },
               &held) != nullptr);

  InterruptManager::unregister_handler(SLOW_VECTOR, &registration);

  // Back only once the handler was let go, and out of it.
  KEXPECT(held.release.load(std::memory_order_acquire));
  KEXPECT(held.out.load(std::memory_order_acquire));
  KEXPECT(wait_for([&] { return held.raised.load(std::memory_order_acquire); }));
}

// Every other CPU raises the vector as fast as it can, while CPU 0 registers and unregisters
// handlers on it at random, in any order, marking each dead the moment it is back. A call that
// finds a dead one was either still running after unregister_handler() returned, or found a
// handler that had been unlinked already.
KTEST_ALL_CPUS(InterruptManager, handlers_come_and_go_while_interrupts_fly, cpu) {
  constexpr Duration RUN = Duration::milliseconds(300);
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> raised{0};
  static std::atomic<std::uint64_t> changes{0};

  if (cpu == 0) {
    stop.store(false);
    raised.store(0);
    changes.store(0);
    g_calls.store(0);
    g_calls_to_the_dead.store(0);
    for (auto& state : g_states) {
      state.store(DEAD);
    }
  }

  sync_cpus();

  if (cpu == 0) {
    std::array<bool, CHURNED> registered{};
    Rng rng{0x1A7E};
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t local_changes = 0;

    while (drivers::SystemClock::now() < end) {
      const std::uint32_t i = static_cast<std::uint32_t>(rng.next() % CHURNED);
      if (registered[i]) {
        InterruptManager::unregister_handler(BUSY_VECTOR, &g_churned[i]);
        g_states[i].store(DEAD, std::memory_order_relaxed);
      } else {
        // Before it is published, which is a release.
        g_states[i].store(LIVE, std::memory_order_relaxed);
        InterruptManager::register_handler(BUSY_VECTOR, &g_churned[i]);
      }

      registered[i] = !registered[i];
      ++local_changes;

      // With one CPU, this one is all there is to raise it.
      raise(BUSY_VECTOR);
    }

    stop.store(true, std::memory_order_release);
    for (std::uint32_t i = 0; i < CHURNED; ++i) {
      if (registered[i]) {
        InterruptManager::unregister_handler(BUSY_VECTOR, &g_churned[i]);
        g_states[i].store(DEAD, std::memory_order_relaxed);
      }
    }

    changes.store(local_changes, std::memory_order_relaxed);
  } else {
    std::uint64_t local_raised = 0;
    while (!stop.load(std::memory_order_acquire)) {
      raise(BUSY_VECTOR);
      ++local_raised;
    }

    raised.fetch_add(local_raised, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(g_calls_to_the_dead.load(), 0u);
    KEXPECT_GT(g_calls.load(), 0u);
    KEXPECT_GT(changes.load(), 100u);
    utils::logger::info("Interrupts: {} changes, {} raised, {} calls\n", changes.load(),
                        raised.load(), g_calls.load());
  }

  sync_cpus();
}
}  // namespace kernel::testing
