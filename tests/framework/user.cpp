// The runner in the user test image: tests on the root task's first thread, or on
// USER_PARTICIPANTS threads at once, each an rt::Thread.
//
// Nothing here can pin a thread to a CPU or keep it from being preempted, unlike the kernel's
// runner, so whoever waits yields: on one CPU, a participant that spun would hold up the very
// ones it waits for until its time slice ran out.
#include <array>
#include <atomic>
#include <utility>

#include "ktest.hpp"
#include "platform.hpp"
#include "rt/thread.hpp"
#include "sys/syscalls.hpp"

namespace ktest {
namespace {
// A lock for the barrier below, which nobody holds for more than a few instructions.
class SpinLock {
  std::atomic<bool> m_locked{false};

 public:
  void lock() noexcept {
    while (m_locked.exchange(true, std::memory_order_acquire)) {
      while (m_locked.load(std::memory_order_relaxed)) {
        user::sys::thread::yield();
      }
    }
  }

  void unlock() noexcept { m_locked.store(false, std::memory_order_release); }
};

// The kernel runner's barrier, for threads: copes with participants that leave the test early.
class Barrier {
  SpinLock m_lock;
  std::uint32_t m_active = 0;
  std::uint32_t m_arrived = 0;
  std::atomic<std::uint64_t> m_generation{0};

  // Under m_lock: lets everybody waiting go on.
  void release() noexcept {
    m_arrived = 0;
    m_generation.fetch_add(1, std::memory_order_release);
  }

 public:
  void reset(const std::uint32_t participants) noexcept {
    m_lock.lock();
    m_active = participants;
    m_arrived = 0;
    m_lock.unlock();
  }

  void arrive() noexcept {
    m_lock.lock();
    const std::uint64_t generation = m_generation.load(std::memory_order_relaxed);
    if (++m_arrived == m_active) {
      release();
      m_lock.unlock();
      return;
    }
    m_lock.unlock();

    while (m_generation.load(std::memory_order_acquire) == generation) {
      user::sys::thread::yield();
    }
  }

  // A participant is done with the test: whoever waits for it no longer has to.
  void leave() noexcept {
    m_lock.lock();
    --m_active;
    if (m_arrived != 0 && m_arrived == m_active) {
      release();
    }
    m_lock.unlock();
  }
};

constinit Barrier g_barrier;
constinit std::uint32_t g_participants = 1;

// Which participant the calling thread is; the first thread is 0.
constinit thread_local std::uint32_t t_participant = 0;

struct Share {
  const detail::TestCase* test;
  std::uint32_t index;
};

void participate(void* const context) noexcept {
  const auto& share = *static_cast<const Share*>(context);
  t_participant = share.index;
  share.test->body(share.index);
  g_barrier.leave();
}
}  // namespace

int run_all() noexcept {
  detail::print("running {} tests\n", detail::registered_tests().size());

  Totals totals = detail::run_tests(detail::Order::Normal);
  totals += detail::run_tests(detail::Order::Last);

  detail::report(totals);
  return totals.failed == 0 ? 0 : 1;
}

std::uint32_t cpu_count() noexcept { return g_participants; }

void sync_cpus() noexcept {
  if (g_participants > 1) {
    g_barrier.arrive();
  }
}

namespace platform {
std::string_view name() noexcept { return "utest"; }

std::string_view participant_kind() noexcept { return "thread"; }

std::uint32_t participant() noexcept { return t_participant; }

// DebugLog makes a line of it on the kernel's console, newline and all.
void write(const std::string_view line) noexcept { (void)user::sys::debug::log(line); }

std::uint64_t now_ns() noexcept { return user::sys::time::now(); }

void run_everywhere(const detail::TestCase& test) noexcept {
  g_participants = USER_PARTICIPANTS;
  g_barrier.reset(USER_PARTICIPANTS);

  // This thread is participant 0, the others are threads of their own, joined below.
  std::array<Share, USER_PARTICIPANTS> shares{};
  std::array<user::rt::Thread, USER_PARTICIPANTS> threads{};
  for (std::uint32_t index = 1; index < USER_PARTICIPANTS; ++index) {
    shares[index] = Share{.test = &test, .index = index};
    auto thread = user::rt::Thread::spawn("utest", &participate, &shares[index]);
    if (!thread) [[unlikely]] {
      // The test runs on fewer; the barrier stops waiting for the rest.
      detail::fail_with(KTEST_HERE, "cannot start participant {}: {}", index,
                        user::sys::to_string(thread.error()));
      g_barrier.leave();
      continue;
    }

    threads[index] = std::move(*thread);
  }

  t_participant = 0;
  test.body(0);
  g_barrier.leave();

  for (user::rt::Thread& thread : threads) {
    if (thread.joinable()) {
      if (const auto joined = thread.join(); !joined) [[unlikely]] {
        detail::fail_with(KTEST_HERE, "a participant could not run: {}",
                          user::sys::to_string(joined.error()));
      }
    }
  }

  g_participants = 1;
}
}  // namespace platform
}  // namespace ktest
