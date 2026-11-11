/// @file object_cap.hpp
/// @brief Threads and processes as capabilities.
///
/// A ThreadCap is made together with its thread, before the thread may run, and from then on it is
/// where the thread's IPC context lives: ThreadCap::current_context() is how the thread gets to it.
/// It learns of the thread's end from the thread's destroy hook, which it puts in front of the one
/// the thread had; the thread holds a reference to it until then. Whatever is done to the thread
/// through it after that finds it gone.
///
/// A ProcessCap holds a reference to its process (see proc/process.hpp), and the process's CSpace,
/// which the threads it starts get for IPC. It borrows the address space for whatever it does to
/// it, so a process that ends meanwhile keeps it until that is done.
///
/// Neither touches the scheduler's side of a thread. A running thread's priority, its CPU or
/// whether it runs at all can only change in ways the scheduler offers, and it offers none of
/// those yet for a thread other than the caller.
#pragma once

#include <chrono.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "core/cap.hpp"
#include "core/cspace.hpp"
#include "core/errors.hpp"
#include "core/ipc/driver_caps.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/message.hpp"
#include "core/ipc/notification.hpp"
#include "hal/user.hpp"
#include "memory/address.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "proc/process.hpp"
#include "sched/thread.hpp"
#include "utils/lock.hpp"

namespace kernel::core::ipc {
/// @brief Labels of the messages a thread's fault handler gets.
enum class FaultMsgLabel : std::uint32_t {
  PageFault = 0xFA01u,      ///< regs: address, access, RIP, thread ID.
  UserException = 0xFA02u,  ///< regs: vector, error code, RIP, thread ID.
  CapFault = 0xFA03u,       ///< regs: the CapId, the error, RIP, thread ID.
};

/// @brief A thread. See the file comment.
class alignas(std::hardware_destructive_interference_size) ThreadCap final
    : public TypedCapObject<ThreadCap, CapType::Thread> {
 public:
  ThreadCap(sched::Thread& thread, CSpace* cspace) noexcept;
  ~ThreadCap() noexcept;

  /// @brief Makes a ThreadCap for @p thread, which gets its IPC context, @p cspace for it, and a
  ///        hook that tells this when the thread is destroyed.
  /// @pre @p thread has not been handed to the scheduler, so that it cannot be gone meanwhile.
  [[nodiscard]] static std::expected<CapRef<ThreadCap>, Error> attach(sched::Thread& thread,
                                                                      CSpace* cspace) noexcept;

  /// @brief Starts a user thread of @p process at @p entry, with a ThreadCap, @p cspace for its
  ///        IPC, and its faults going to @p fault_endpoint, if there is one, with @p fault_badge.
  [[nodiscard]] static std::expected<CapRef<ThreadCap>, Error> spawn_user(
      proc::Process& process, CSpace* cspace, std::string_view name, memory::VirtualAddress entry,
      std::uint64_t arg = 0, const sched::UserThreadCreateOptions& opts = {},
      Endpoint* fault_endpoint = nullptr, std::uint64_t fault_badge = 0) noexcept;

  /// @brief Starts a kernel thread with a ThreadCap, and @p cspace for its IPC.
  [[nodiscard]] static std::expected<CapRef<ThreadCap>, Error> create_kernel_thread(
      std::string_view name, sched::Thread::RawEntryFn entry, void* arg, CSpace* cspace,
      const sched::ThreadCreateOptions& opts = {}) noexcept;

  /// @brief The IPC context of the calling thread, if it has a ThreadCap.
  [[nodiscard]] static ThreadIpcContext* current_context() noexcept {
    return sched::current()->ipc_context();
  }

  /// @brief Whether the thread is still there.
  [[nodiscard]] bool is_alive() const noexcept;

  [[nodiscard]] sched::ThreadId tid() const noexcept { return m_tid; }

  /// @brief The thread's IPC context. For the thread itself, and for setting things up before it
  ///        runs.
  [[nodiscard]] ThreadIpcContext& ipc_context() noexcept { return m_ipc; }

  /// @brief Asks the thread to end (sched::Thread::request_exit()). Takes Write.
  [[nodiscard]] std::expected<void, Error> request_exit(CapRights rights) const noexcept;

  /// @brief What the scheduler counted for the thread so far. Takes Read.
  [[nodiscard]] std::expected<sched::ThreadStats, Error> read_stats(
      CapRights rights) const noexcept;

  /// @brief Binds @p notification to the thread (Notification::bind()). Takes Manage.
  [[nodiscard]] std::expected<void, Error> bind_notification(CapRights rights,
                                                             Notification& notification) noexcept;

  /// @brief Unbinds whatever is bound. Takes Manage.
  [[nodiscard]] std::expected<void, Error> unbind_notification(CapRights rights) noexcept;

  /// @brief Where the thread's faults go: @p endpoint, with @p badge. Null for nowhere. Takes
  ///        Manage.
  [[nodiscard]] std::expected<void, Error> set_fault_endpoint(CapRights rights, Endpoint* endpoint,
                                                              std::uint64_t badge) noexcept;

  /// @brief Tells the fault handler about a fault, and waits for its answer: a call on the fault
  ///        endpoint. @pre The calling thread is this one, and can sleep.
  /// @return NotFound without a fault endpoint; whatever the handler answered with otherwise.
  [[nodiscard]] std::expected<void, Error> send_fault(FaultMsgLabel label, std::uint64_t arg0,
                                                      std::uint64_t arg1,
                                                      std::uint64_t rip) noexcept;

 private:
  static void on_thread_destroyed(sched::Thread* thread, void* self) noexcept;

  mutable utils::QSpinlock m_lock{};
  sched::Thread* m_thread;  ///< Null once destroyed. Under the lock.
  sched::ThreadId m_tid;
  sched::Thread::DestroyHookFn m_next_hook{nullptr};
  void* m_next_cookie{nullptr};

  Endpoint* m_fault_endpoint{nullptr};  ///< With a reference. Under the lock.
  std::uint64_t m_fault_badge{0};

  IpcBuffer m_buffer{};
  ThreadIpcContext m_ipc;
};

/// @brief A process. See the file comment.
class alignas(std::hardware_destructive_interference_size) ProcessCap final
    : public TypedCapObject<ProcessCap, CapType::Process> {
 public:
  /// @pre The caller holds a reference to @p process, which this takes one of its own of.
  ProcessCap(proc::Process& process, CapRef<CSpace> cspace) noexcept;
  ~ProcessCap() noexcept;

  /// @brief A new process, under @p parent, with a new CSpace.
  [[nodiscard]] static std::expected<CapRef<ProcessCap>, Error> create(
      std::string_view name, proc::Process* parent = nullptr) noexcept;

  /// @brief A ProcessCap for @p process, with @p cspace, or if null a new CSpace, which counts
  ///        against the process's quota.
  [[nodiscard]] static std::expected<CapRef<ProcessCap>, Error> wrap(
      proc::Process& process, CSpace* cspace = nullptr) noexcept;

  /// @brief Starts a user thread at @p entry, with a ThreadCap, the process's CSpace, and its
  ///        fault endpoint. Takes Write and Manage.
  [[nodiscard]] std::expected<CapRef<ThreadCap>, Error> spawn_thread(
      CapRights rights, std::string_view name, memory::VirtualAddress entry, std::uint64_t arg = 0,
      const sched::UserThreadCreateOptions& opts = {}) const noexcept;

  /// @brief proc::Process::fork() of @p parent, with a ProcessCap for the child, which gets a
  ///        CSpace of its own, and a ThreadCap for its thread, with that CSpace and
  ///        @p fault_endpoint, if there is one, so that the thread has an IPC context from the
  ///        start. @pre The caller keeps @p parent's address space there while it runs.
  [[nodiscard]] static std::expected<CapRef<ProcessCap>, Error> fork_from(
      proc::Process& parent, std::string_view name, memory::VirtualAddress entry,
      std::uint64_t arg = 0, const sched::UserThreadCreateOptions& opts = {},
      Endpoint* fault_endpoint = nullptr, std::uint64_t fault_badge = 0) noexcept;

  /// @brief proc::Process::fork_here() of @p parent: a child whose thread carries on where the
  ///        calling thread is, at @p at, as a copy of it.
  ///
  /// The child gets a ProcessCap, and a copy of @p caller's CSpace, slot for slot (see
  /// CSpace::copy_into()), so that every capability the caller had is there under the ID it knew
  /// it by. Its thread gets a ThreadCap, with that CSpace, the IpcBuffer the caller named, which
  /// the address space has a copy of, and @p fault_endpoint, if there is one.
  /// @pre The calling thread is a user thread of @p parent, and @p caller is its IPC context.
  [[nodiscard]] static std::expected<CapRef<ProcessCap>, Error> fork_here(
      proc::Process& parent, std::string_view name, const hw::user::ResumeFrame& at,
      const ThreadIpcContext& caller, Endpoint* fault_endpoint = nullptr,
      std::uint64_t fault_badge = 0) noexcept;

  /// @brief fork_from() this process, with its fault endpoint. Takes Write and Manage.
  [[nodiscard]] std::expected<CapRef<ProcessCap>, Error> fork(
      CapRights rights, std::string_view name, memory::VirtualAddress entry, std::uint64_t arg = 0,
      const sched::UserThreadCreateOptions& opts = {}) const noexcept;

  /// @brief proc::Process::exit(). Takes Write and Manage. Not for process 0.
  [[nodiscard]] std::expected<void, Error> terminate(CapRights rights, int status) const noexcept;

  /// @brief proc::Process::wait_child(), with its preconditions. Takes Read.
  [[nodiscard]] std::expected<proc::ProcessId, Error> wait_child(
      CapRights rights, proc::ProcessId target = proc::ANY_CHILD,
      int* status = nullptr) const noexcept;

  /// @brief wait_child() that gives up after @p timeout, with Error::Timeout. Takes Read.
  [[nodiscard]] std::expected<proc::ProcessId, Error> wait_child_for(
      CapRights rights, proc::ProcessId target, klib::Duration timeout,
      int* status = nullptr) const noexcept;

  /// @brief Where faults of threads started from now on go. Takes Manage.
  [[nodiscard]] std::expected<void, Error> set_fault_endpoint(CapRights rights, Endpoint* endpoint,
                                                              std::uint64_t badge) noexcept;

  /// @brief Anonymous memory in the process. Takes Write and Manage.
  [[nodiscard]] std::expected<memory::VirtualAddress, Error> allocate(
      CapRights rights, std::size_t size, memory::vmm::AccessFlags access,
      memory::vmm::PageSize page_size = memory::vmm::PageSize::Size4K,
      memory::VirtualAddress hint = memory::VirtualAddress{}) const noexcept;

  /// @brief Maps the SharedVmObject @p object names in @p source into the process. Takes Write
  ///        on this; the object's capability decides what @p access may be.
  [[nodiscard]] std::expected<memory::VirtualAddress, Error> map_shared(
      CapRights rights, const CSpace& source, CapId object, memory::vmm::AccessFlags access,
      std::size_t first_page = 0, std::size_t pages = 0,
      memory::VirtualAddress hint = memory::VirtualAddress{}) const noexcept;

  /// @brief Maps the MmioRegion @p region names in @p source into the process, for user code.
  ///        Takes Write and Manage on this; the region's capability decides whether it is
  ///        writable.
  [[nodiscard]] std::expected<memory::VirtualAddress, Error> map_mmio(
      CapRights rights, const CSpace& source, CapId region, std::size_t offset = 0,
      std::size_t length = 0,
      memory::VirtualAddress hint = memory::VirtualAddress{}) const noexcept;

  /// @brief AddressSpace::protect() in the process. Takes Write and Manage.
  [[nodiscard]] std::expected<void, Error> protect(CapRights rights, memory::VirtualAddress address,
                                                   std::size_t size,
                                                   memory::vmm::AccessFlags access) const noexcept;

  /// @brief Unmaps whatever starts at @p address. Takes Write.
  [[nodiscard]] std::expected<void, Error> unmap(CapRights rights,
                                                 memory::VirtualAddress address) const noexcept;

  /// @brief Copies, or mints with @p badge, the capability @p cap of @p source into the process's
  ///        CSpace, with the rights in @p mask. Takes Grant.
  [[nodiscard]] std::expected<CapId, Error> grant(CapRights rights, const CSpace& source, CapId cap,
                                                  CapRights mask = CapRights::All,
                                                  std::uint64_t badge = 0) const noexcept;

  [[nodiscard]] proc::ProcessId pid() const noexcept { return m_pid; }
  [[nodiscard]] proc::Process& process() const noexcept { return *m_process; }
  [[nodiscard]] CSpace& cspace() const noexcept { return *m_cspace; }

 private:
  // The address space, borrowed for as long as this lives.
  class SpaceLoan;

  // The ProcessCap for a child just forked, with @p cspace and the fault endpoint its thread has.
  // A child that cannot have one is ended.
  [[nodiscard]] static std::expected<CapRef<ProcessCap>, Error> wrap_forked(
      proc::Process& child, CSpace& cspace, Endpoint* fault_endpoint,
      std::uint64_t fault_badge) noexcept;

  proc::Process* m_process;  ///< With a reference.
  proc::ProcessId m_pid;
  CapRef<CSpace> m_cspace;

  mutable utils::QSpinlock m_lock{};
  Endpoint* m_fault_endpoint{nullptr};  ///< With a reference. Under the lock.
  std::uint64_t m_fault_badge{0};
};
}  // namespace kernel::core::ipc
