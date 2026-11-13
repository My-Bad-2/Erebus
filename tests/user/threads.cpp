// The Thread family from user code, and threads of one process running at once.
#include <array>
#include <atomic>
#include <cstdint>

#include "ktest.hpp"
#include "rt/thread.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
constexpr std::uint64_t MILLISECOND = 1'000'000;

// What CapDelete takes for the caller's own CSpace.
constexpr sys::CapId OWN_CSPACE = sys::NULL_CAP;

std::atomic<std::uint64_t> g_counter{0};

// Spins in ring 3 until it is ended from outside, which only an IPI can bring about.
void spin_forever(std::uint64_t) noexcept {
  while (true) {
    asm volatile("pause");
  }
}
}  // namespace

KTEST(Threads, each_has_an_id_of_its_own) {
  static std::atomic<sys::ThreadId> s_theirs{0};
  const sys::ThreadId mine = sys::thread::id();
  KEXPECT_NE(mine, 0u);
  KEXPECT_EQ(sys::thread::id(), mine);

  auto thread =
      rt::Thread::spawn("id", [](void*) noexcept { s_theirs.store(sys::thread::id()); }, nullptr);
  KREQUIRE_OK(thread);
  KEXPECT_OK(thread->join());
  KEXPECT_NE(s_theirs.load(), 0u);
  KEXPECT_NE(s_theirs.load(), mine);
}

KTEST(Threads, stats_count_what_the_thread_did) {
  const auto before = sys::thread::stats();
  KREQUIRE_OK(before);
  KEXPECT_LE(before->creation_time_ns, sys::time::now());

  for (int i = 0; i < 3; ++i) {
    KEXPECT_OK(sys::time::sleep_for(MILLISECOND));
  }

  const auto after = sys::thread::stats();
  KREQUIRE_OK(after);
  KEXPECT_GE(after->voluntary_switches, before->voluntary_switches + 3);
  KEXPECT_GE(after->switch_count, before->switch_count + 3);
  KEXPECT_GE(after->total_runtime_ns, before->total_runtime_ns);
  KEXPECT_EQ(after->creation_time_ns, before->creation_time_ns);
}

KTEST(Threads, stats_of_another_come_through_its_capability) {
  auto thread = rt::Thread::spawn(
      "stats", [](void*) noexcept { (void)sys::time::sleep_for(MILLISECOND); }, nullptr);
  KREQUIRE_OK(thread);

  const auto stats = sys::thread::stats(thread->cap());
  KEXPECT_OK(stats);
  KEXPECT_OK(thread->join());

  // An ID a CSpace could have but this one does not, and one no CSpace could have.
  KEXPECT_ERROR(sys::thread::stats(sys::CapId::make(0xFF00, 1)), sys::Error::NotFound);
  KEXPECT_ERROR(sys::thread::stats(sys::CapId::make(0xFFFFF, 1)), sys::Error::InvalidArgument);
}

// A thread spinning in ring 3 on a CPU of its own never comes into the kernel by itself: nothing
// else there arms a timer. So ending it takes an interrupt, which the exit has to send. One
// spinner per participant, given a moment for idle CPUs to take them, leaves at least one alone
// on a CPU wherever there is more than one.
//
// They end one at a time, each given time for what its end sets off to be over before the next
// is asked to: a thread that ends gives back its stack, and the TLB shootdown for that interrupts
// every CPU the process runs on, which would end the next spinner whether its exit did its part
// or not. Nothing here blocks, for the same reason: a CPU that went idle sends IPIs of its own.
KTEST(Threads, exit_through_a_capability_ends_threads_spinning_in_ring_3) {
  std::array<sys::CapId, ktest::USER_PARTICIPANTS> spinners{};
  for (sys::CapId& thread_id : spinners) {
    const auto thread = sys::thread::spawn("spinner", &spin_forever);
    KREQUIRE_OK(thread);
    thread_id = *thread;
  }

  const auto spin_for = [](const std::uint64_t ns) {
    const std::uint64_t until = sys::time::now() + ns;
    while (sys::time::now() < until) {
      asm volatile("pause");
    }
  };

  spin_for(20 * MILLISECOND);
  for (const sys::CapId thread_id : spinners) {
    KEXPECT_OK(sys::thread::stats(thread_id));  // still there
    KEXPECT_OK(sys::thread::exit(thread_id));

    // Gone once its stats say so: that is when it has been destroyed.
    const std::uint64_t deadline = sys::time::now() + 500 * MILLISECOND;
    sys::Result<sys::ThreadStats> stats = sys::thread::stats(thread_id);
    while (stats && sys::time::now() < deadline) {
      stats = sys::thread::stats(thread_id);
    }
    KEXPECT_ERROR(stats, sys::Error::InvalidState);
    KEXPECT_OK(sys::cap::remove(OWN_CSPACE, thread_id));

    spin_for(10 * MILLISECOND);
  }
}

KTEST(Threads, yield_comes_back) {
  for (int i = 0; i < 100; ++i) {
    sys::thread::yield();
  }
  KEXPECT_NE(sys::thread::id(), 0u);
}

// Several threads of one process at once, on as many CPUs as there are: the counter is exact if
// nobody's increment was lost, which takes every CPU seeing the same memory.
KTEST_ALL_CPUS(Threads, run_at_once_on_shared_memory, cpu) {
  constexpr std::uint64_t ROUNDS = 20'000;
  if (cpu == 0) {
    g_counter.store(0);
  }
  sync_cpus();

  for (std::uint64_t i = 0; i < ROUNDS; ++i) {
    g_counter.fetch_add(1, std::memory_order_relaxed);
    if (i % 1024 == 0) {
      sys::thread::yield();
    }
  }
  sync_cpus();

  KEXPECT_EQ(g_counter.load(), ROUNDS * cpu_count());
}
}  // namespace user::testing
