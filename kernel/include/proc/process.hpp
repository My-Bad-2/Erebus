/// @file process.hpp
/// @brief Processes: an address space, the user threads that run in it, and a place in a tree.
///
/// A process owns a user address space and some number of user threads (sched::Thread created
/// with Thread::create_user()). Process 0 is the kernel itself: it has no address space and no
/// threads of its own, and it is the parent of every process the kernel starts, and of every
/// process whose parent went away first.
///
/// ### The way out
///
/// exit() can come from anywhere, and asks every thread of the process to stop. A thread does on
/// its next way back to ring 3 (sched::Scheduler::return_to_user()), which comes soon: a thread in
/// ring 3 gets an IPI to bring it into the kernel. A fault in user code that nothing could resolve
/// is an exit() too, with EXIT_FAULT.
///
/// Once the last thread is destroyed, by `kreaperd`, the process lets go of its address space,
/// hands its children to process 0, and becomes a zombie: just the status, for wait_child() to
/// collect. A process nobody will wait for, one that lost its parent, is freed right there. Doing
/// this when the last thread is destroyed rather than when it exits is what makes it safe: by
/// then, no thread can be on its way through the address space any more.
///
/// ### References
///
/// Code outside the tree that keeps a process, a capability to it say, holds a reference with
/// retain(). The memory stays until the last one goes, however the process ended: such a holder
/// finds it Dead, with no address space, and nothing to do to it. The address space itself is
/// borrowed with acquire_address_space(), and goes when the process is done and the last
/// borrower has given it back.
///
/// ### Locks
///
/// Each process has a lock for its threads and its state. Parents and children change under one
/// lock for the whole tree: an exit touches three processes at once, the one leaving, its parent
/// and its children, and wait_child() looks at all the children, which would need an order for
/// those locks that nobody has to get wrong this way. It is only taken when a process starts or
/// ends, or someone waits for one.
#pragma once

#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>
#include <expected>
#include <string_view>

#include "core/errors.hpp"
#include "memory/address.hpp"
#include "sched/thread.hpp"
#include "sched/wait_queue.hpp"
#include "sched/workqueue.hpp"
#include "utils/linked_list.hpp"
#include "utils/lock.hpp"

namespace kernel::memory::vmm {
class AddressSpace;
}

namespace kernel::proc {
using ProcessId = std::uint32_t;

/// @brief The kernel's own process.
inline constexpr ProcessId KERNEL_PID = 0;

/// @brief For wait_child(): whichever child is done first.
inline constexpr ProcessId ANY_CHILD = ~0u;

/// @brief The exit status of a process whose user code faulted and was ended for it.
inline constexpr int EXIT_FAULT = -1;

/// @brief Where user address spaces start handing out addresses. Low enough for both regions of
///        an AddressSpace to fit below the end of the lower half, high enough that small
///        addresses, null and what is just above it, never map anything.
inline constexpr std::uintptr_t USER_SPACE_BASE = 0x0000'0040'0000'0000;

enum class ProcessState : std::uint8_t {
  Running,  ///< Its threads run, or it has none yet.
  Stopped,  ///< Reserved for when something can stop a process. Nothing does yet.
  Exiting,  ///< exit() came. Threads are on their way out, and no new ones start.
  Zombie,   ///< Every thread is gone, and so is the address space. Waits for its parent.
  Dead,     ///< Collected. The object is about to go away.
};

/// @brief Which list a Process's SListNode base is for.
struct ProcessChildTag {};

/// @brief A process. See the file comment.
class alignas(std::hardware_destructive_interference_size) Process
    : public utils::SListNode<Process, ProcessChildTag> {
 public:
  static constexpr std::size_t MAX_NAME_LEN = 24;

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  /// @brief Creates a process with an empty address space, and no threads yet.
  /// @param parent Null for the process of the calling thread, which is process 0 for a kernel
  ///               thread.
  /// @return The process, or Error::InvalidState if the parent is exiting already, or
  ///         Error::OutOfMemory.
  [[nodiscard]] static auto create(std::string_view name, Process* parent = nullptr) noexcept
      -> std::expected<Process*, Error>;

  /// @brief Starts a user thread at @p entry, with @p arg in RDI. See sched::Thread::create_user().
  /// @return The thread, which must not be touched once it may have ended, or
  ///         Error::InvalidState for a process that is exiting or is process 0.
  [[nodiscard]] auto spawn_thread(std::string_view name, memory::VirtualAddress entry,
                                  std::uint64_t arg = 0,
                                  const sched::UserThreadCreateOptions& opts = {}) noexcept
      -> std::expected<sched::Thread*, Error>;

  /// @brief What spawn_thread() runs on the new thread before it may run: the last moment anything
  ///        about it can be set up without racing it. An error ends the thread then and there.
  using ThreadSetup = std::expected<void, Error> (*)(sched::Thread& thread, void* context) noexcept;

  /// @brief spawn_thread(), with @p setup run on the thread first.
  [[nodiscard]] auto spawn_thread(std::string_view name, memory::VirtualAddress entry,
                                  std::uint64_t arg, const sched::UserThreadCreateOptions& opts,
                                  ThreadSetup setup, void* setup_context) noexcept
      -> std::expected<sched::Thread*, Error>;

  /// @brief Makes a child with a copy of this address space, private memory shared copy-on-write,
  ///        and starts one thread in it at @p entry.
  ///
  /// There is no system call to return from yet, so the child does not carry on where a thread of
  /// this one is, but where it is told to. Everything else is what fork() means: the child sees
  /// the memory as it was, and what either side writes from now on is its own.
  [[nodiscard]] auto fork(std::string_view name, memory::VirtualAddress entry,
                          std::uint64_t arg = 0,
                          const sched::UserThreadCreateOptions& opts = {}) noexcept
      -> std::expected<Process*, Error>;

  /// @brief Ends the process with @p status, from anywhere but an NMI. Only the first call counts.
  ///
  /// Returns before the threads are gone. wait_child() on the parent is what says when they are.
  void exit(int status) noexcept;

  /// @brief Waits for the child @p target, or any child for ANY_CHILD, to be a zombie, collects
  ///        its status and frees it.
  /// @pre Called from a thread that can sleep, usually one of this process, or a kernel thread
  ///      for process 0. The children have to have been created by it, or by someone who leaves
  ///      the waiting to it.
  /// @return The child's ID, or Error::NotFound if there is no such child, or none at all.
  [[nodiscard]] auto wait_child(ProcessId target, int* out_status = nullptr) noexcept
      -> std::expected<ProcessId, Error>;

  /// @brief wait_child() that gives up after @p timeout, with Error::Timeout.
  [[nodiscard]] auto wait_child_for(ProcessId target, klib::Duration timeout,
                                    int* out_status = nullptr) noexcept
      -> std::expected<ProcessId, Error>;

  [[nodiscard]] ProcessId pid() const noexcept { return m_pid; }
  [[nodiscard]] std::string_view name() const noexcept { return std::string_view{m_name.data()}; }

  [[nodiscard]] ProcessState state() const noexcept {
    return m_state.load(std::memory_order_acquire);
  }

  /// @brief The address space, or null for process 0 and for one that is done.
  [[nodiscard]] memory::vmm::AddressSpace* address_space() const noexcept {
    return m_address_space;
  }

  /// @brief Threads that have not been destroyed yet. Out of date as soon as it is read.
  [[nodiscard]] std::uint32_t live_threads() const noexcept {
    return m_live_threads.load(std::memory_order_relaxed);
  }

  /// @brief What exit() was given. Meaningful once the process is exiting.
  [[nodiscard]] int exit_status() const noexcept { return m_exit_status; }

  /// @brief Keeps the process's memory until release(). @pre The caller has a reference already,
  ///        or got the process from somewhere that keeps it alive, its tree say.
  void retain() noexcept { m_refs.fetch_add(1, std::memory_order_relaxed); }

  /// @brief Lets go of a reference. The last one frees the process, which is Dead by then.
  void release() noexcept;

  /// @brief Borrows the address space, which stays until release_address_space(), however the
  ///        process ends meanwhile. @return Null for one that is done, or process 0.
  [[nodiscard]] memory::vmm::AddressSpace* acquire_address_space() noexcept;

  /// @brief Gives back what acquire_address_space() lent.
  void release_address_space() noexcept { m_space_users.fetch_sub(1, std::memory_order_release); }

 private:
  Process() noexcept = default;
  ~Process() noexcept = default;

  [[nodiscard]] static Process* allocate(std::string_view name) noexcept;
  static void free(Process* process) noexcept;

  static void on_thread_destroyed(sched::Thread* thread, void* process) noexcept;
  static void finish_later(void* process) noexcept;
  void finish() noexcept;
  void discard() noexcept;

  // Takes the address space away, waits for whoever borrowed it, and destroys it.
  void destroy_address_space() noexcept;
  [[nodiscard]] bool reap_locked(ProcessId target, int* out_status, ProcessId& found,
                                 Process*& zombie) noexcept;
  [[nodiscard]] bool has_child_locked(ProcessId target) const noexcept;

  friend void initialize() noexcept;

  ProcessId m_pid{KERNEL_PID};
  std::atomic<ProcessState> m_state{ProcessState::Running};
  memory::vmm::AddressSpace* m_address_space{nullptr};  ///< Taken away under m_lock.
  std::atomic<std::uint32_t> m_space_users{0};          ///< See acquire_address_space().
  std::atomic<std::uint32_t> m_refs{1};                 ///< The tree's, and retain()'s.
  int m_exit_status{0};

  // Threads, and the state changes that have to agree with them, under m_lock.
  utils::QSpinlock m_lock{};
  utils::SList<sched::Thread, sched::ProcessThreadTag> m_threads{};
  std::atomic<std::uint32_t> m_live_threads{0};

  // The tree, under the lock for all of it (see the file comment).
  Process* m_parent{nullptr};
  utils::SList<Process, ProcessChildTag> m_children{};
  bool m_detached{false};  ///< Nobody waits for it, so it goes as soon as it is done.

  sched::WaitQueue m_child_exit_wq{};  ///< Waiting for a child to be done.
  sched::WorkItem m_finish{&finish_later, this};
  std::array<char, MAX_NAME_LEN> m_name{};
};

/// @brief Process 0. See the file comment.
[[nodiscard]] Process* kernel_process() noexcept;

/// @brief The process of the calling thread, which is process 0 for a kernel thread.
[[nodiscard]] Process* current_process() noexcept;

/// @brief Creates process 0, and has page faults and other exceptions in user code come here.
/// @pre The heap, the scheduler and the workqueues work. Called once.
void initialize() noexcept;
}  // namespace kernel::proc
