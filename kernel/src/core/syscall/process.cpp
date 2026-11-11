#include "proc/process.hpp"

#include <algorithm>
#include <array>
#include <chrono.hpp>
#include <limits>

#include "core/ipc/object_cap.hpp"
#include "hal/user.hpp"
#include "handlers.hpp"

namespace kernel::core::syscall::handlers {
namespace {
// Puts a capability to a child, which is running already, in the caller's CSpace. A child its
// parent could never get at would run on with nobody to manage it, so one that cannot be handed
// over is ended. Its parent can still collect it by its process ID.
[[nodiscard]] std::expected<ipc::CapId, Error> hand_over(
    ipc::CSpace& cspace, const ipc::CapRef<ipc::ProcessCap>& child) noexcept {
  auto id = cspace.insert(*child, ipc::CapRights::All);
  if (!id) [[unlikely]] {
    child->process().exit(proc::EXIT_FAULT);
  }

  return id;
}

// Nanoseconds from user code, which go as far as a Duration can.
[[nodiscard]] klib::Duration nanoseconds(const std::uint64_t raw) noexcept {
  constexpr auto LONGEST = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return klib::Duration::nanoseconds(static_cast<std::int64_t>(std::min(raw, LONGEST)));
}
}  // namespace

std::int64_t proc_create(Frame& frame) noexcept {
  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  std::array<char, proc::Process::MAX_NAME_LEN> buffer{};
  const auto name = copy_string_from_user(frame.address<0>(), frame.arg<1>(), buffer);
  if (!name) [[unlikely]] {
    return encode_error(name.error());
  }

  // Null makes it a child of the calling process. Another parent takes a capability that may
  // manage it, which also keeps it from going away meanwhile.
  proc::Process* parent = nullptr;
  ipc::Held<ipc::ProcessCap> held_parent{};
  if (const ipc::CapId parent_id = frame.cap<2>(); !parent_id.is_null()) {
    auto acquired = (*cspace)->acquire<ipc::ProcessCap>(
        parent_id, ipc::CapRights::Write | ipc::CapRights::Manage);
    if (!acquired) [[unlikely]] {
      return encode_error(acquired.error());
    }

    held_parent = std::move(*acquired);
    parent = &held_parent.object->process();
  }

  const auto child = ipc::ProcessCap::create(*name, parent);
  if (!child) [[unlikely]] {
    return encode_error(child.error());
  }

  return to_rax((*cspace)->insert(**child, ipc::CapRights::All));
}

std::int64_t proc_fork(Frame& frame) noexcept {
  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  std::array<char, proc::Process::MAX_NAME_LEN> buffer{};
  const auto name = copy_string_from_user(frame.address<0>(), frame.arg<1>(), buffer);
  if (!name) [[unlikely]] {
    return encode_error(name.error());
  }

  proc::Process* const self = proc::current_process();
  if (self == nullptr) [[unlikely]] {
    return encode_error(Error::InvalidState);
  }

  // The calling thread keeps the address space there while it is copied. The child's thread has
  // an IPC context, with the child's own CSpace, from the start; it has no fault endpoint, as there
  // is no capability here to take one from, until ThreadConfigure gives it one.
  const auto child = ipc::ProcessCap::fork_from(*self, *name, frame.address<2>(), frame.arg<3>());
  if (!child) [[unlikely]] {
    return encode_error(child.error());
  }

  return to_rax(hand_over(**cspace, *child));
}

// ProcForkHere: a0 and a1 name the child, as for ProcFork. It returns twice. In the caller, with a
// capability to the child in RAX and the child's process ID in RDI. In the child, whose one thread
// carries on from here, with 0 in both and every other register as the call came in; RCX and R11
// hold what SYSCALL left in them, as on any return. The child has a copy of the caller's address
// space and of its CSpace, slot for slot, so that the same IDs name the same objects, and of the
// calling thread's IPC buffer, FPU and vector state, FS and GS bases, and PKRU. Other threads of
// the caller are not copied, and neither is a one-shot capability. Like ProcFork, the child has no
// fault endpoint until ThreadConfigure gives it one.
std::int64_t proc_fork_here(Frame& frame) noexcept {
  const auto ctx = caller_context();
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  std::array<char, proc::Process::MAX_NAME_LEN> buffer{};
  const auto name = copy_string_from_user(frame.address<0>(), frame.arg<1>(), buffer);
  if (!name) [[unlikely]] {
    return encode_error(name.error());
  }

  proc::Process* const self = proc::current_process();
  if (self == nullptr || self == proc::kernel_process()) [[unlikely]] {
    return encode_error(Error::InvalidState);
  }

  const UserReturn& from = frame.from();
  hw::user::ResumeFrame at{
      .regs = frame.registers(), .rip = from.rip, .rsp = from.rsp, .rflags = from.rflags};
  at.regs.rax = 0;
  at.regs.rdi = 0;
  at.regs.rcx = from.rip;
  at.regs.r11 = from.rflags;

  const auto child = ipc::ProcessCap::fork_here(*self, *name, at, **ctx);
  if (!child) [[unlikely]] {
    return encode_error(child.error());
  }

  const auto id = hand_over(*(*ctx)->cspace(), *child);
  if (!id) [[unlikely]] {
    return encode_error(id.error());
  }

  frame.set_second_result((*child)->pid());
  return to_rax(id);
}

std::int64_t proc_exit(Frame& frame) noexcept {
  const auto status = static_cast<int>(frame.arg<1>());

  if (frame.cap<0>().is_null()) [[likely]] {
    proc::Process* const self = proc::current_process();
    if (self == nullptr || self == proc::kernel_process()) [[unlikely]] {
      return encode_error(Error::InvalidState);
    }

    // The thread goes back toward ring 3, and sched::Scheduler::return_to_user() ends it there.
    self->exit(status);
    return 0;
  }

  return to_rax(caller_cspace().and_then([&](const ipc::CSpace* cspace) {
    return cspace->acquire<ipc::ProcessCap>(frame.cap<0>()).and_then([&](const auto& process) {
      return process.object->terminate(process.rights, status);
    });
  }));
}

std::int64_t proc_wait(Frame& frame) noexcept {
  const auto target = static_cast<proc::ProcessId>(frame.arg<1>());
  const std::uint64_t timeout = frame.arg<2>();
  const memory::VirtualAddress status_at = frame.address<3>();

  // Checked first: once a child is collected, it cannot be waited for again.
  if (!status_at.is_null() && !hw::user::is_user_range(status_at, sizeof(int))) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  int status = 0;
  std::expected<proc::ProcessId, Error> collected = std::unexpected(Error::InvalidState);

  if (frame.cap<0>().is_null()) [[likely]] {
    proc::Process* const self = proc::current_process();
    if (self == nullptr) [[unlikely]] {
      return encode_error(Error::InvalidState);
    }

    collected = timeout == 0 ? self->wait_child(target, &status)
                             : self->wait_child_for(target, nanoseconds(timeout), &status);
  } else {
    const auto cspace = caller_cspace();
    const auto process = cspace.and_then(
        [&](const ipc::CSpace* own) { return own->acquire<ipc::ProcessCap>(frame.cap<0>()); });
    if (!process) [[unlikely]] {
      return encode_error(process.error());
    }

    collected = timeout == 0 ? process->object->wait_child(process->rights, target, &status)
                             : process->object->wait_child_for(process->rights, target,
                                                               nanoseconds(timeout), &status);
  }

  if (!collected) [[unlikely]] {
    return encode_error(collected.error());
  }

  // The child is gone either way. Memory that went away under the status loses only the status.
  if (!status_at.is_null()) {
    (void)copy_to_user(status_at, status);
  }

  return to_rax(collected);
}

std::int64_t proc_id(Frame&) noexcept {
  const proc::Process* const self = proc::current_process();
  if (self == nullptr) [[unlikely]] {
    return encode_error(Error::InvalidState);
  }

  return self->pid();
}

std::int64_t proc_grant(const Frame& frame) noexcept {
  const auto mask = rights_arg(frame.arg<2>());
  const auto cspace = caller_cspace();
  if (const Error error = first_error(mask, cspace); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  const auto process = (*cspace)->acquire<ipc::ProcessCap>(frame.cap<0>());
  if (!process) [[unlikely]] {
    return encode_error(process.error());
  }

  return to_rax(
      process->object->grant(process->rights, **cspace, frame.cap<1>(), *mask, frame.arg<3>()));
}
}  // namespace kernel::core::syscall::handlers
