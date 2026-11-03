#include <atomic>

#include "drivers/clock.hpp"
#include "ktest.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
constinit utils::RWLock g_rw;
std::atomic<bool> g_writer_in{false};

constinit utils::QSpinlock g_outer;
constinit utils::QSpinlock g_inner;
std::uint64_t g_counter = 0;
constexpr std::uint64_t LOCK_ROUNDS = 100'000;
}  // namespace

// CPU 0 holds a read lock for a while; CPU 1 asks for the write lock meanwhile and must not get
// it until the reader is gone.
KTEST_ALL_CPUS(RWLock, keeps_a_writer_out_while_a_reader_holds_it, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("needs a second CPU to be the writer");
  }

  if (cpu == 0) {
    g_writer_in.store(false, std::memory_order_relaxed);
    g_rw.read_lock();
  }
  sync_cpus();

  if (cpu == 1) {
    g_rw.write_lock();
    g_writer_in.store(true, std::memory_order_release);
    g_rw.write_unlock();
  }

  if (cpu == 0) {
    drivers::SystemClock::spin_wait(klib::Duration::milliseconds(50));
    const bool writer_got_in = g_writer_in.load(std::memory_order_acquire);
    g_rw.read_unlock();
    KEXPECT(!writer_got_in);
  }
  sync_cpus();

  if (cpu == 0) {
    KEXPECT(g_writer_in.load(std::memory_order_acquire));
  }
}

// Every CPU bumps a plain counter under the lock, half the time under a second, nested one.
// A lost update means two CPUs were inside at once.
KTEST_ALL_CPUS(QSpinlock, keeps_a_counter_exact_under_contention, cpu) {
  if (cpu == 0) {
    g_counter = 0;
  }
  sync_cpus();

  for (std::uint64_t i = 0; i < LOCK_ROUNDS; ++i) {
    utils::NakedGuard outer{g_outer};
    if (i & 1) {
      utils::NakedGuard inner{g_inner};
      g_counter = g_counter + 1;
    } else {
      g_counter = g_counter + 1;
    }
  }
  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(g_counter, LOCK_ROUNDS * cpu_count());
  }
}
}  // namespace kernel::testing
