#include "sched/thread.hpp"

#include <array>

#include "core/ipc/notification.hpp"
#include "core/ipc/object_cap.hpp"
#include "handlers.hpp"
#include "proc/process.hpp"
#include "sched/scheduler.hpp"

namespace kernel::core::syscall::handlers {
std::int64_t thread_spawn(Frame& frame) noexcept {
  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  std::array<char, sched::Thread::MAX_NAME_LEN> buffer{};
  const auto name = copy_string_from_user(frame.address<1>(), frame.arg<2>(), buffer);
  if (!name) [[unlikely]] {
    return encode_error(name.error());
  }

  const memory::VirtualAddress entry = frame.address<3>();
  const std::uint64_t arg = frame.arg<4>();

  std::expected<ipc::CapRef<ipc::ThreadCap>, Error> thread = std::unexpected(Error::InvalidState);
  if (frame.cap<0>().is_null()) [[likely]] {
    // A thread of the caller's own process shares the caller's CSpace. It has no fault endpoint
    // until ThreadConfigure gives it one, as there is no capability here to take one from.
    proc::Process* const self = proc::current_process();
    if (self == nullptr || self == proc::kernel_process()) [[unlikely]] {
      return encode_error(Error::InvalidState);
    }

    thread = ipc::ThreadCap::spawn_user(*self, *cspace, *name, entry, arg);
  } else {
    const auto process = (*cspace)->acquire<ipc::ProcessCap>(frame.cap<0>());
    if (!process) [[unlikely]] {
      return encode_error(process.error());
    }

    thread = process->object->spawn_thread(process->rights, *name, entry, arg);
  }

  if (!thread) [[unlikely]] {
    return encode_error(thread.error());
  }

  return to_rax((*cspace)->insert(**thread, ipc::CapRights::All));
}

std::int64_t thread_exit(Frame& frame) noexcept {
  if (frame.cap<0>().is_null()) [[likely]] {
    // sched::Scheduler::return_to_user() ends it on the way back.
    sched::current()->request_exit();
    return 0;
  }

  return to_rax(caller_cspace().and_then([&](const ipc::CSpace* cspace) {
    return cspace->acquire<ipc::ThreadCap>(frame.cap<0>()).and_then([](const auto& thread) {
      return thread.object->request_exit(thread.rights);
    });
  }));
}

std::int64_t thread_configure(Frame& frame) noexcept {
  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  const ipc::CapId notification_id = frame.cap<1>();
  const ipc::CapId fault_id = frame.cap<2>();
  const std::uint64_t fault_badge = frame.arg<3>();

  ipc::Held<ipc::Endpoint> fault_endpoint{};
  if (!fault_id.is_null()) {
    auto endpoint = (*cspace)->acquire<ipc::Endpoint>(fault_id, ipc::CapRights::Write);
    if (!endpoint) [[unlikely]] {
      return encode_error(endpoint.error());
    }

    fault_endpoint = std::move(*endpoint);
  }

  auto thread = (*cspace)->acquire<ipc::ThreadCap>(frame.cap<0>());
  if (thread) {
    if (!notification_id.is_null()) {
      const auto notification =
          (*cspace)->acquire<ipc::Notification>(notification_id, ipc::CapRights::Read);
      if (!notification) [[unlikely]] {
        return encode_error(notification.error());
      }

      if (const auto bound =
              thread->object->bind_notification(thread->rights, *notification->object);
          !bound) [[unlikely]] {
        return encode_error(bound.error());
      }
    }

    return to_rax(thread->object->set_fault_endpoint(thread->rights, fault_endpoint.object.get(),
                                                     fault_badge));
  }

  // Not a thread: a capability of another type answers InvalidArgument, and then it may be a
  // process, whose threads have no notification to bind.
  if (thread.error() != Error::InvalidArgument) [[unlikely]] {
    return encode_error(thread.error());
  }

  if (!notification_id.is_null()) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  const auto process = (*cspace)->acquire<ipc::ProcessCap>(frame.cap<0>());
  if (!process) [[unlikely]] {
    return encode_error(process.error());
  }

  return to_rax(process->object->set_fault_endpoint(process->rights, fault_endpoint.object.get(),
                                                    fault_badge));
}

std::int64_t thread_yield(Frame&) noexcept {
  sched::Scheduler::yield();
  return 0;
}

std::int64_t thread_id(Frame&) noexcept {
  return static_cast<std::int64_t>(sched::current()->tid());
}

std::int64_t thread_stats(Frame& frame) noexcept {
  const memory::VirtualAddress out = frame.address<1>();

  std::expected<sched::ThreadStats, Error> stats = std::unexpected(Error::InvalidState);
  if (frame.cap<0>().is_null()) [[likely]] {
    stats = sched::current()->stats();
  } else {
    stats = caller_cspace().and_then([&](const ipc::CSpace* cspace) {
      return cspace->acquire<ipc::ThreadCap>(frame.cap<0>()).and_then([](const auto& thread) {
        return thread.object->read_stats(thread.rights);
      });
    });
  }

  return to_rax(
      stats.and_then([out](const sched::ThreadStats& value) { return copy_to_user(out, value); }));
}
}  // namespace kernel::core::syscall::handlers
