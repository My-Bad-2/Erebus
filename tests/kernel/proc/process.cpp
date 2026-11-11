// Processes without any user code: how they start, end, and get collected, and who looks after a
// process that lost its parent.
#include "proc/process.hpp"

#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "drivers/clock.hpp"
#include "hal/user.hpp"
#include "ktest.hpp"
#include "sched/sleep.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using proc::Process;
using proc::ProcessState;
}  // namespace

KTEST(Process, starts_with_an_address_space_and_a_parent) {
  const auto created = Process::create("fresh");
  KREQUIRE(created.has_value());
  Process* process = *created;

  KEXPECT_NE(process->pid(), proc::KERNEL_PID);
  KEXPECT(process->name() == "fresh");
  KEXPECT(process->state() == ProcessState::Running);
  KEXPECT(process->address_space() != nullptr);
  KEXPECT_EQ(process->live_threads(), 0u);

  // A kernel thread's process is process 0, which has nothing of the kind.
  KEXPECT(proc::current_process() == proc::kernel_process());
  KEXPECT(proc::kernel_process()->address_space() == nullptr);

  process->exit(0);
  KEXPECT(proc::kernel_process()->wait_child(process->pid()).has_value());
}

KTEST(Process, its_status_goes_to_whoever_waits_for_it) {
  const auto created = Process::create("status");
  KREQUIRE(created.has_value());
  const proc::ProcessId pid = (*created)->pid();

  (*created)->exit(42);
  (*created)->exit(7);  // only the first one counts

  int status = 0;
  const auto reaped = proc::kernel_process()->wait_child(pid, &status);
  KREQUIRE(reaped.has_value());
  KEXPECT_EQ(*reaped, pid);
  KEXPECT_EQ(status, 42);

  // Collected once, and gone after that.
  const auto again = proc::kernel_process()->wait_child(pid);
  KREQUIRE(!again);
  KEXPECT(again.error() == Error::NotFound);
}

KTEST(Process, waiting_for_a_child_that_is_still_there_takes_as_long_as_it_says) {
  const auto created = Process::create("alive");
  KREQUIRE(created.has_value());

  const auto waited =
      proc::kernel_process()->wait_child_for((*created)->pid(), Duration::milliseconds(10));
  KREQUIRE(!waited);
  KEXPECT(waited.error() == Error::Timeout);

  (*created)->exit(0);
  KEXPECT(proc::kernel_process()->wait_child((*created)->pid()).has_value());
}

// A test body is an idle thread, which never really sleeps, so the parent that waits here is a
// thread of its own: one that does sleep, and has to be woken when the child is done.
KTEST(Process, a_waiting_parent_is_woken_when_its_child_is_done) {
  const auto created = Process::create("awaited");
  KREQUIRE(created.has_value());

  struct Parent {
    proc::ProcessId child{0};
    int status{0};
    bool reaped{false};
    klib::Duration took{};
    std::atomic<bool> waiting{false};
    std::atomic<bool> done{false};
  } parent{.child = (*created)->pid()};

  KREQUIRE(support::start(
               "parent",
               +[](Parent* p) noexcept {
                 p->waiting.store(true, std::memory_order_release);
                 const klib::TimePoint start = drivers::SystemClock::now();
                 const auto reaped = proc::kernel_process()->wait_child_for(
                     p->child, Duration::seconds(10), &p->status);
                 p->took = drivers::SystemClock::now() - start;
                 p->reaped = reaped.has_value();
                 p->done.store(true, std::memory_order_release);
               },
               &parent) != nullptr);

  // Long enough for it to be asleep, not merely on its way.
  KEXPECT(support::wait_for([&] { return parent.waiting.load(std::memory_order_acquire); }));
  (void)sched::sleep_for(Duration::milliseconds(20));
  (*created)->exit(4);

  KEXPECT(support::wait_for([&] { return parent.done.load(std::memory_order_acquire); }));
  KEXPECT(parent.reaped);
  KEXPECT_EQ(parent.status, 4);
  KEXPECT(parent.took < Duration::seconds(5));
}

KTEST(Process, any_child_will_do) {
  const auto first = Process::create("first");
  const auto second = Process::create("second", *first);
  KREQUIRE(first.has_value());
  KREQUIRE(second.has_value());

  // Nobody else's children: the second is the first's, and the first has nothing else.
  const proc::ProcessId pid = (*second)->pid();
  (*second)->exit(3);

  int status = 0;
  const auto reaped = (*first)->wait_child(proc::ANY_CHILD, &status);
  KREQUIRE(reaped.has_value());
  KEXPECT_EQ(*reaped, pid);
  KEXPECT_EQ(status, 3);

  const auto nothing_left = (*first)->wait_child(proc::ANY_CHILD);
  KREQUIRE(!nothing_left);
  KEXPECT(nothing_left.error() == Error::NotFound);

  (*first)->exit(0);
  KEXPECT(proc::kernel_process()->wait_child((*first)->pid()).has_value());
}

// The parent goes first. Process 0 takes the child, and frees it when it is done, as nobody is
// going to wait for it.
KTEST(Process, an_orphan_is_looked_after) {
  const auto parent = Process::create("parent");
  KREQUIRE(parent.has_value());
  const auto child = Process::create("child", *parent);
  KREQUIRE(child.has_value());
  const proc::ProcessId child_pid = (*child)->pid();

  (*parent)->exit(0);
  KEXPECT(proc::kernel_process()->wait_child((*parent)->pid()).has_value());

  // Freed the moment it is done, so there is nothing to wait for.
  (*child)->exit(9);
  const auto waited = proc::kernel_process()->wait_child(child_pid);
  KREQUIRE(!waited);
  KEXPECT(waited.error() == Error::NotFound);
}

// A zombie parent gave its children away already, and would never see a new one.
KTEST(Process, a_zombie_gets_no_children) {
  const auto parent = Process::create("zombie");
  KREQUIRE(parent.has_value());
  (*parent)->exit(0);
  KEXPECT((*parent)->state() == ProcessState::Zombie);

  const auto child = Process::create("late", *parent);
  KREQUIRE(!child);
  KEXPECT(child.error() == Error::InvalidState);

  KEXPECT(proc::kernel_process()->wait_child((*parent)->pid()).has_value());
}

KTEST(Process, an_exiting_process_starts_no_threads) {
  const auto created = Process::create("closed");
  KREQUIRE(created.has_value());
  (*created)->exit(0);

  const auto thread = (*created)->spawn_thread("late", memory::VirtualAddress{0x400000});
  KREQUIRE(!thread);
  KEXPECT(thread.error() == Error::InvalidState);
  KEXPECT(proc::kernel_process()->wait_child((*created)->pid()).has_value());

  // Process 0 has no address space to run anything in.
  const auto kernel_thread =
      proc::kernel_process()->spawn_thread("nope", memory::VirtualAddress{0x400000});
  KREQUIRE(!kernel_thread);
  KEXPECT(kernel_thread.error() == Error::InvalidState);
}

KTEST(Process, user_threads_only_start_where_user_code_can_be) {
  const auto created = Process::create("bounds");
  KREQUIRE(created.has_value());

  for (const std::uintptr_t entry : {0x0ul, 0x10ul, 0xffff'8000'0000'0000ul}) {
    const auto thread = (*created)->spawn_thread("bad", memory::VirtualAddress{entry});
    KREQUIRE(!thread);
    KEXPECT(thread.error() == Error::InvalidArgument);
  }

  KEXPECT_EQ((*created)->live_threads(), 0u);
  (*created)->exit(0);
  KEXPECT(proc::kernel_process()->wait_child((*created)->pid()).has_value());
}

// Many at once, all of them collected, from several waiters.
KTEST(Process, comes_and_goes_many_times) {
  constexpr std::uint32_t PROCESSES = 32;
  proc::ProcessId pids[PROCESSES]{};

  for (std::uint32_t i = 0; i < PROCESSES; ++i) {
    const auto created = Process::create("many");
    KREQUIRE(created.has_value());
    pids[i] = (*created)->pid();
    (*created)->exit(static_cast<int>(i));
  }

  for (std::uint32_t i = PROCESSES; i-- > 0;) {
    int status = -1;
    KEXPECT(proc::kernel_process()->wait_child(pids[i], &status).has_value());
    KEXPECT_EQ(status, static_cast<int>(i));
  }
}

// A copy of a thread is of the thread that asks, in ring 3, into a process forked from its own:
// a kernel thread has nothing to copy, and no place outside user space can be carried on from.
KTEST(Process, only_a_user_thread_is_copied_and_only_from_user_space) {
  const auto created = Process::create("copied");
  KREQUIRE(created.has_value());

  hw::user::ResumeFrame at{};
  at.rip = 0x400000;
  at.rsp = 0x500000;
  at.rflags = hw::user::INITIAL_RFLAGS;
  KEXPECT(sched::Thread::create_user_copy("copy", *created, at).error() == Error::InvalidState);
  KEXPECT((*created)->fork_here("copy", at).error() == Error::InvalidState);

  at.rip = 0xffff'8000'0000'0000ul;
  KEXPECT(sched::Thread::create_user_copy("copy", *created, at).error() == Error::InvalidArgument);
  at.rip = 0x400000;
  at.rsp = 0x0000'8000'0000'0000ul;
  KEXPECT(sched::Thread::create_user_copy("copy", *created, at).error() == Error::InvalidArgument);

  KEXPECT_EQ((*created)->live_threads(), 0u);
  (*created)->exit(0);
  KEXPECT(proc::kernel_process()->wait_child((*created)->pid()).has_value());
}
}  // namespace kernel::testing
