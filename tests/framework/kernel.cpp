// The runner in the test kernel: tests on the BSP, or on every CPU at once, the user test image
// in between, and the verdict for QEMU.
#include <atomic>
#include <chrono.hpp>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/io.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "platform.hpp"
#include "proc/process.hpp"
#include "sched/scheduler.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::testing {
namespace {
using ::ktest::Totals;
using ::ktest::detail::TestCase;

// How long the user test image may take. It needs a second or two, but a TCG boot of an old CPU
// model is many times slower than KVM.
constexpr auto USER_TESTS_TIMEOUT = klib::Duration::seconds(120);

// What a process that would not end in time gets after it was told to: one of its threads may be
// blocked in IPC, which an exit request does not reach yet.
constexpr auto USER_TESTS_GRACE = klib::Duration::seconds(5);

std::uint32_t g_participants = 1;

// A barrier for the CPUs of a multi-CPU test, which copes with CPUs that leave the test early.
class Barrier {
  utils::QSpinlock m_lock;
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
    utils::NakedGuard guard{m_lock};
    m_active = participants;
    m_arrived = 0;
  }

  void arrive() noexcept {
    std::uint64_t generation = 0;
    {
      utils::NakedGuard guard{m_lock};
      generation = m_generation.load(std::memory_order_relaxed);
      if (++m_arrived == m_active) {
        release();
        return;
      }
    }

    while (m_generation.load(std::memory_order_acquire) == generation) {
      hw::pause();
    }
  }

  // A CPU is done with the test: whoever waits for it no longer has to.
  void leave() noexcept {
    utils::NakedGuard guard{m_lock};
    --m_active;
    if (m_arrived != 0 && m_arrived == m_active) {
      release();
    }
  }
};

constinit Barrier g_barrier;

// The test the CPUs run at the moment. Bumped by the BSP to start a multi-CPU test on every AP.
constinit const TestCase* g_shared_test = nullptr;
std::atomic<std::uint64_t> g_round{0};
std::atomic<std::uint32_t> g_started_aps{0};
std::atomic<std::uint32_t> g_finished_cpus{0};
std::atomic<bool> g_run_over{false};

void run_share(const std::uint32_t cpu) noexcept {
  g_shared_test->body(cpu);
  g_barrier.leave();
  g_finished_cpus.fetch_add(1, std::memory_order_release);
}

// Ends QEMU when it has an isa-debug-exit device at the port: it exits with (code << 1) | 1.
// Elsewhere nothing listens there, and the write goes nowhere.
void report_to_emulator([[maybe_unused]] const bool passed) noexcept {
#ifdef EREBUS_KTEST_EXIT_PORT
  hw::IoResource{hw::IoType::PMIO, EREBUS_KTEST_EXIT_PORT}.write<std::uint32_t>(0, passed ? 0 : 1);
#endif
}

// The user test image, started and waited for as one more test.
constinit StartUserTests g_start_user_tests = nullptr;

void run_user_image(std::uint32_t) noexcept {
  proc::Process* const process = g_start_user_tests != nullptr ? g_start_user_tests() : nullptr;
  if (process == nullptr) {
    ::ktest::detail::fail(KTEST_HERE, "the boot modules have no user test image to run");
    return;
  }

  // The process stays until it is collected, so it can be told to end until then.
  const proc::ProcessId pid = process->pid();
  int status = 0;
  auto collected = proc::kernel_process()->wait_child_for(pid, USER_TESTS_TIMEOUT, &status);
  if (!collected && collected.error() == Error::Timeout) {
    ::ktest::detail::fail_with(KTEST_HERE, "it did not finish within {} s",
                               USER_TESTS_TIMEOUT.to_ms() / 1000);
    process->exit(proc::EXIT_FAULT);
    collected = proc::kernel_process()->wait_child_for(pid, USER_TESTS_GRACE, &status);
    if (!collected) {
      ::ktest::detail::fail_with(KTEST_HERE, "it did not end when told to either");
    }

    return;
  }

  if (!collected) {
    ::ktest::detail::fail_with(KTEST_HERE, "cannot wait for it: error {:#x}",
                               std::to_underlying(collected.error()));
    return;
  }

  // 1 is what the image says when a test of its own failed, and it has said which already.
  if (status != 0) {
    ::ktest::detail::fail_with(KTEST_HERE, "it exited with status {}", status);
  }
}

constexpr TestCase USER_IMAGE{
    .suite = "UserSpace",
    .name = "every_user_test_passes",
    .body = &run_user_image,
    .mode = ::ktest::detail::Mode::Bsp,
    .order = ::ktest::detail::Order::Normal,
};
}  // namespace

void run_all(const StartUserTests start_user_tests) noexcept {
  ::ktest::detail::print("running {} tests on {} CPUs\n",
                         ::ktest::detail::registered_tests().size() + 1, hw::smp::online_cpus());

  // The user image comes before the last tests, which may leave the other CPUs stopped for good.
  g_start_user_tests = start_user_tests;
  Totals totals = ::ktest::detail::run_tests(::ktest::detail::Order::Normal);
  totals += ::ktest::detail::run_one(USER_IMAGE);
  totals += ::ktest::detail::run_tests(::ktest::detail::Order::Last);

  g_run_over.store(true, std::memory_order_release);

  ::ktest::detail::report(totals);
  report_to_emulator(totals.failed == 0);
}

void run_on_ap() noexcept {
  const std::uint32_t cpu = hw::percpu::id();
  std::uint64_t seen = 0;

  while (!g_run_over.load(std::memory_order_acquire)) {
    const std::uint64_t round = g_round.load(std::memory_order_acquire);
    if (round == seen) {
      // Between tests, this is an ordinary idle CPU, asleep until something wakes it. Tests hand
      // threads to the scheduler, and those have to get to run somewhere: the user image's too.
      sched::Scheduler::idle_step();
      continue;
    }

    seen = round;
    g_started_aps.fetch_add(1, std::memory_order_acq_rel);
    run_share(cpu);
  }
}
}  // namespace kernel::testing

namespace ktest {
std::uint32_t cpu_count() noexcept { return kernel::testing::g_participants; }

void sync_cpus() noexcept {
  if (kernel::testing::g_participants > 1) {
    kernel::testing::g_barrier.arrive();
  }
}

namespace platform {
std::string_view name() noexcept { return "ktest"; }

std::string_view participant_kind() noexcept { return "CPU"; }

std::uint32_t participant() noexcept { return kernel::hw::percpu::id(); }

// Straight to the console, without the logger's level and location prefix: every line already
// says where it comes from.
void write(const std::string_view line) noexcept { kernel::utils::logger::write_console(line); }

std::uint64_t now_ns() noexcept { return kernel::drivers::SystemClock::now().raw_ns(); }

void run_everywhere(const detail::TestCase& test) noexcept {
  using namespace kernel;
  using namespace kernel::testing;

  g_shared_test = &test;
  g_participants = hw::smp::online_cpus();
  g_barrier.reset(g_participants);
  g_finished_cpus.store(0, std::memory_order_relaxed);
  g_started_aps.store(0, std::memory_order_relaxed);
  g_round.fetch_add(1, std::memory_order_release);

  // The APs are asleep in their idle loops. An IPI can land just before an AP halts, after it took
  // its last look at the round, so this keeps knocking until they have all answered.
  const std::uint32_t me = hw::percpu::id();
  while (g_started_aps.load(std::memory_order_acquire) != g_participants - 1) {
    hw::smp::for_each_online([me](const std::uint32_t cpu) {
      if (cpu != me) {
        hw::ipi::send_reschedule(cpu);
      }
    });

    drivers::SystemClock::spin_wait(klib::Duration::microseconds(50));
  }

  run_share(me);

  while (g_finished_cpus.load(std::memory_order_acquire) != g_participants) {
    hw::pause();
  }

  g_participants = 1;
}
}  // namespace platform
}  // namespace ktest
