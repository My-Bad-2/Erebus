// Processes from user code: forking both ways, exit statuses, waiting, and faults.
#include <atomic>
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
// Where a child made by ProcFork starts. It has no thread-local storage, so it sticks to calls
// that need none, and it has an empty CSpace.
[[noreturn]] void exit_with_arg(const std::uint64_t arg) noexcept {
  (void)sys::proc::exit(static_cast<int>(arg));
  __builtin_trap();
}

// Somewhere in the first 4 MiB, which a user image never has mapped.
constexpr std::uintptr_t UNMAPPED = 0x2000;

std::uint64_t g_before_fork = 0;
}  // namespace

KTEST(Process, has_an_id_of_its_own) {
  const auto pid = sys::proc::id();
  KREQUIRE_OK(pid);
  KEXPECT_NE(*pid, 0u);  // process 0 is the kernel
}

KTEST(Process, fork_here_returns_in_both) {
  const auto forked = sys::proc::fork_here("both");
  KREQUIRE_OK(forked);
  if (forked->is_child()) {
    (void)sys::proc::exit(23);
    __builtin_trap();
  }

  KEXPECT(forked->is_parent());
  KEXPECT_NE(forked->child_pid, 0u);

  int status = 0;
  const auto waited = sys::proc::wait(forked->child_pid, &status);
  KEXPECT_OK(waited);
  KEXPECT_EQ(waited.value_or(0), forked->child_pid);
  KEXPECT_EQ(status, 23);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, forked->child_process));
}

// The child gets a copy of memory as it was, and what either writes afterwards stays its own.
KTEST(Process, a_child_writes_to_its_own_copy_of_memory) {
  g_before_fork = 0xC0FFEE;

  const auto status = in_child("copy", [] {
    const bool saw_it = g_before_fork == 0xC0FFEE;
    g_before_fork = 0xDEAD;
    return saw_it && g_before_fork == 0xDEAD ? 0 : 1;
  });

  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, 0);
  KEXPECT_EQ(g_before_fork, 0xC0FFEEu);
}

// Its CSpace is a copy too, slot for slot: the same IDs name the same objects.
KTEST(Process, a_child_has_the_same_capabilities) {
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(notification);
  const sys::CapId id = *notification;

  const auto status = in_child("caps", [id] { return sys::ipc::signal(id) ? 0 : 1; });
  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, 0);

  const auto bits = sys::ipc::poll(id);
  KEXPECT_OK(bits);
  KEXPECT_EQ(bits.value_or(0), 1u);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, id));
}

KTEST(Process, fork_starts_a_child_at_an_entry_point) {
  const auto child = sys::proc::fork("entry", &exit_with_arg, 77);
  KREQUIRE_OK(child);

  int status = 0;
  KEXPECT_OK(sys::proc::wait(sys::ANY_CHILD, &status));
  KEXPECT_EQ(status, 77);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *child));
}

KTEST(Process, a_child_that_faults_ends_with_the_fault_status) {
  const auto status = in_child("faults", [] {
    return static_cast<int>(*reinterpret_cast<volatile std::uint8_t*>(UNMAPPED));
  });

  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, -1);  // proc::EXIT_FAULT
}

KTEST(Process, wait_gives_up_after_its_timeout_and_exit_ends_the_child) {
  const auto forked = sys::proc::fork_here("waits");
  KREQUIRE_OK(forked);
  if (forked->is_child()) {
    // Until it is told to stop. A thread in ring 3 gets an IPI for that, so it spins there.
    while (true) {
      asm volatile("pause");
    }
  }

  KEXPECT_ERROR(sys::proc::wait(forked->child_pid, nullptr, 20'000'000), sys::Error::Timeout);

  KEXPECT_OK(sys::proc::exit(31, forked->child_process));
  int status = 0;
  KEXPECT_OK(sys::proc::wait(forked->child_pid, &status));
  KEXPECT_EQ(status, 31);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, forked->child_process));
}

KTEST(Process, waiting_with_no_child_finds_none) {
  KEXPECT_ERROR(sys::proc::wait(sys::ANY_CHILD), sys::Error::NotFound);
  KEXPECT_ERROR(sys::proc::wait(0), sys::Error::NotFound);
}

KTEST(Process, wait_refuses_a_status_outside_user_space_before_it_collects) {
  const auto forked = sys::proc::fork_here("status");
  KREQUIRE_OK(forked);
  if (forked->is_child()) {
    (void)sys::proc::exit(4);
    __builtin_trap();
  }

  auto* const kernel_half = reinterpret_cast<int*>(0xFFFF'8000'0000'0000);
  KEXPECT_ERROR(sys::proc::wait(forked->child_pid, kernel_half), sys::Error::InvalidArgument);

  // Refused before anything was done: the child is still there to collect.
  int status = 0;
  KEXPECT_OK(sys::proc::wait(forked->child_pid, &status));
  KEXPECT_EQ(status, 4);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, forked->child_process));
}

KTEST(Process, create_makes_an_empty_child_that_can_be_given_capabilities) {
  const auto child = sys::proc::create("empty");
  KREQUIRE_OK(child);

  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(notification);

  // The child's CSpace is empty, so the copy goes in a slot of its own there.
  const auto granted = sys::proc::grant(*child, *notification, sys::CapRights::Read);
  KEXPECT_OK(granted);
  KEXPECT_NE(granted.value_or(sys::NULL_CAP), sys::NULL_CAP);

  // Without Grant on the process capability, nothing can be given.
  const auto weak = sys::cap::copy(sys::NULL_CAP, *child, sys::NULL_CAP,
                                   sys::CapRights::Write | sys::CapRights::Manage);
  KREQUIRE_OK(weak);
  KEXPECT_ERROR(sys::proc::grant(*weak, *notification), sys::Error::AccessDenied);

  // It never ran, and ends when told to.
  const auto pid_of_nothing = sys::proc::wait(sys::ANY_CHILD, nullptr, 1'000'000);
  KEXPECT_ERROR(pid_of_nothing, sys::Error::Timeout);
  KEXPECT_OK(sys::proc::exit(9, *child));

  int status = 0;
  KEXPECT_OK(sys::proc::wait(sys::ANY_CHILD, &status));
  KEXPECT_EQ(status, 9);

  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *weak));
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *child));
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *notification));
}
}  // namespace user::testing
