/// @file thread.hpp
/// @brief Kernel threads, and switching between them.
///
/// A Thread is a kernel stack plus whatever else the CPU needs to carry on where it left off.
/// This file is the mechanism: switch_to() moves a CPU from one thread to another. Deciding when,
/// and to which one, is up to sched::Scheduler (see sched/scheduler.hpp).
///
/// Every CPU gets a Thread around the stack it booted on before it goes online, so current()
/// is never null from then on. That thread is the CPU's idle thread. Everything else comes from
/// create_kernel() or create_user(), starts out `Ready`, and first runs when the scheduler is
/// told about it. When its entry function returns it turns `Dead`, calls its exit hook if it has
/// one, and leaves the CPU for good; `kreaperd` then destroys it (see sched/reaper.hpp).
///
/// A user thread is a kernel thread whose entry function never returns: it goes to ring 3 and
/// only comes back on interrupts. It belongs to a proc::Process, runs in that process's address
/// space, and has a stack there as well as its kernel stack. It can only be stopped from the
/// outside, with request_exit(), which it acts on the next time it is about to go back to user
/// mode (see Scheduler::return_to_user()).
///
/// Code that wants to drive switch_to() by hand can: a thread nobody gave to the scheduler is left
/// alone by it. Such a thread stays the business of whoever switches to it, and has to be switched
/// away from in its exit hook and destroyed by hand. Just do not mix the two on one thread.
///
/// FPU state is opt-in. The kernel itself stays off those registers (see hal/fpu.hpp), so a
/// thread needs a save area only if it uses the FPU in a utils::FpuGuard or asks for one, and
/// the memory that takes, and the XSAVE on every switch, only go to threads that do.
#pragma once

#include <array>
#include <atomic>
#include <bitfield.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <string_view>
#include <type_traits>

#include "core/errors.hpp"
#include "hal/fpu.hpp"
#include "hal/hal.hpp"
#include "memory/address.hpp"
#include "memory/quota.hpp"
#include "sched/eevdf.hpp"
#include "sched/priority.hpp"
#include "utils/linked_list.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::core::ipc {
class ThreadIpcContext;
}

namespace kernel::proc {
class Process;
}

namespace kernel::hw::user {
struct ResumeFrame;
}

namespace kernel::sched {
using ThreadId = std::uint64_t;
inline constexpr ThreadId INVALID_THREAD_ID{0};

inline constexpr std::size_t DEFAULT_KSTACK_SIZE = 32 * 1024;  // 32 KiB
inline constexpr std::size_t MIN_KSTACK_SIZE = 16 * 1024;      // 16 KiB
inline constexpr std::size_t STACK_ALIGNMENT = 16;

/// @brief For ThreadCreateOptions: the scheduler can choose.
inline constexpr std::uint32_t NO_PREFERENCE = ~0u;

/// @brief Which list a Thread's SListNode base is for.
struct RunQueueTag {};
struct ReapQueueTag {};
struct ProcessThreadTag {};  ///< The threads of a proc::Process.

/// @brief The link a Mutex has on the list of its owner, see Thread::m_contended_mutexes.
struct HeldMutexTag {};

/// @brief Thread execution lifecycle states.
enum class ThreadState : std::uint8_t {
  Created = 0,   ///< Allocated and initialized, but not yet enqueued or switched in
  Ready = 1,     ///< Runnable and waiting to be scheduled onto a CPU
  Running = 2,   ///< Currently executing on a CPU Core
  Blocked = 3,   ///< Waiting on a synchronization primitive
  Sleeping = 4,  ///< Waiting for a monotonic timer deadline
  Dead = 5,      ///< Finished execution. Awaiting stack/TCB reclamation
};

/// @brief Thread flags.
///
/// Bit Layout:
///   [0..3]    : ThreadState
///   [4..7]    : PriorityClass
///   [8]       : is_kthread (1 = kernel thread, 0 = user thread)
///   [9]       : is_idle (1 = per-CPU idle bootstrap thread)
///   [10]      : is_pinned (1 = bound to a specific CPU core)
///   [11]      : uses_fpu (1 = thread has a save area for its FPU/SSE/AVX state)
///   [12]      : fpu_dirty (1 = the CPU registers hold FPU state the save area does not have)
///   [13]      : on_runqueue (1 = waiting on a scheduler runqueue, so not the running one)
///   [14]      : stack_borrowed (1 = uses pre-allocated boot stack; do not vmm->free on exit)
///   [15]      : exit_requested (1 = cooperative cancellation / stop signal pending)
///   [16..23]  : nice_bias (signed 8-bit nice offset encoded as biased [0..39])
///   [24]      : uses_amx (1 = thread explicitly opted into 8 KiB of Intel AMX tile data)
///   [25]      : in_kernel_fpu (1 = thread is currently inside a Ring-0 FPU guard scope)
///   [26]      : scheduled (1 = handed to the scheduler, which owns it from then on)
///   [27]      : migrating (1 = between two run queues, so on neither of them for a moment)
///   [28]      : sleep_cancelled (1 = cancel_sleep() came by, for the next sleep to pick up)
class ThreadFlags {
  std::uint32_t m_data{0};

 public:
  constexpr explicit ThreadFlags(const std::uint32_t raw = 0) noexcept : m_data(raw) {}

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return m_data; }
  constexpr void clear() noexcept { m_data = 0; }

  BF_RW(ThreadState, state, 0, 4)
  BF_RW(PriorityClass, priority_class, 4, 4)
  BF_BIT_RW(is_kthread, 8)
  BF_BIT_RW(is_idle, 9)
  BF_BIT_RW(is_pinned, 10)
  BF_BIT_RW(uses_fpu, 11)
  BF_BIT_RW(fpu_dirty, 12)
  BF_BIT_RW(on_runqueue, 13)
  BF_BIT_RW(stack_borrowed, 14)
  BF_BIT_RW(exit_requested, 15)
  BF_RW(std::uint8_t, nice_bias, 16, 8)
  BF_BIT_RW(uses_amx, 24)
  BF_BIT_RW(in_kernel_fpu, 25)
  BF_BIT_RW(scheduled, 26)
  BF_BIT_RW(migrating, 27)
  BF_BIT_RW(sleep_cancelled, 28)
};

static_assert(sizeof(ThreadFlags) == sizeof(std::uint32_t));

/// @brief SysV AMD64 ABI callee-saved register frame pushed onto the outgoing thread's stack during
/// a cooperative or preemptive context switch.
///
/// Layout is ABI-locked with `sched::switch_context`:
///   rsp + 0x00 : r15
///   rsp + 0x08 : r14
///   rsp + 0x10 : r13
///   rsp + 0x18 : r12
///   rsp + 0x20 : rbx
///   rsp + 0x28 : rbp
///   rsp + 0x30 : rip (return address popped by `retq`)
struct SwitchContext {
  std::uint64_t r15{0};
  std::uint64_t r14{0};
  std::uint64_t r13{0};
  std::uint64_t r12{0};
  std::uint64_t rbx{0};
  std::uint64_t rbp{0};
  std::uint64_t rip{0};

  [[nodiscard]] constexpr bool operator==(const SwitchContext&) const noexcept = default;
};

static_assert(sizeof(SwitchContext) == 7 * sizeof(std::uint64_t), "SwitchContext must be 56 bytes");
static_assert(offsetof(SwitchContext, r15) == 0x00);
static_assert(offsetof(SwitchContext, r14) == 0x08);
static_assert(offsetof(SwitchContext, r13) == 0x10);
static_assert(offsetof(SwitchContext, r12) == 0x18);
static_assert(offsetof(SwitchContext, rbx) == 0x20);
static_assert(offsetof(SwitchContext, rbp) == 0x28);
static_assert(offsetof(SwitchContext, rip) == 0x30);

/// @brief Architectural thread state that sits outside gprs: TLS bases, Memory Protection keys,
/// Extended feature disable, selective XSAVE Component Mask, and 64-byte-aligned tiered SIMD state.
struct ArchitecturalState {
  std::uint64_t fs_base{0};                   ///< User TLS base
  std::uint64_t user_gs_base{0};              ///< User GS base
  std::uint64_t xsave_mask{0};                ///< What fpu_storage holds, as hw::fpu::state_mask()
  hw::fpu::Ia32Xfd xfd{hw::fpu::XFD_NO_AMX};  ///< IA32_XFD MSR state
  std::uint32_t pkru{0};                      ///< Protection Key Rights for User pages
  std::size_t fpu_buffer_size{0};             ///< Allocated size
  void* fpu_storage{nullptr};                 ///< Save area, see hal/fpu.hpp
};

/// @brief Per-thread execution & profiling statistics tracked at ns resolution.
struct ThreadStats {
  std::uint64_t switch_count{0};          ///< Total times scheduled onto a CPU
  std::uint64_t voluntary_switches{0};    ///< Cooperative yields / blocks
  std::uint64_t involuntary_switches{0};  ///< Preemptions
  std::uint64_t total_runtime_ns{0};      ///< Cumulative CPU execution time
  std::uint64_t last_switch_in_ns{0};     ///< Timestamp of last switch-in
  std::uint64_t creation_time_ns{0};      ///< Timestamp when TCB was constructed
};

/// @brief Configuration options when creating a new Thread.
struct ThreadCreateOptions {
  std::size_t stack_size{DEFAULT_KSTACK_SIZE};  ///< A multiple of STACK_ALIGNMENT; rounded up to
                                                ///< whole pages.
  PriorityClass priority{PriorityClass::Normal};
  std::uint32_t preferred_cpu{NO_PREFERENCE};  ///< Starts here; the creator's CPU if none.
  std::uint32_t preferred_numa_node{
      NO_PREFERENCE};                        ///< Stays near here; near its last CPU if none.
  std::uint64_t slice_ns{DEFAULT_SLICE_NS};  ///< Requested EEVDF time slice.
  bool pinned{false};        ///< Only ever runs on `preferred_cpu`, which has to be set then.
  bool allocate_fpu{false};  ///< Gives the thread a save area from the start
  bool allocate_amx{false};  ///< The same with room for Intel AMX tile data; fails without AMX
  std::uint64_t initial_fs_base{0};
};

/// @brief Configuration options when creating a user thread.
///
/// A user thread always has a save area: its registers are the user's, and have to survive
/// whatever the kernel does in between.
struct UserThreadCreateOptions {
  std::size_t kernel_stack_size{DEFAULT_KSTACK_SIZE};  ///< As ThreadCreateOptions::stack_size.
  std::size_t user_stack_size{2 * 1024 * 1024};        ///< Demand paged, and it can grow.
  PriorityClass priority{PriorityClass::Normal};
  std::uint32_t preferred_cpu{NO_PREFERENCE};
  std::uint32_t preferred_numa_node{NO_PREFERENCE};
  std::uint64_t slice_ns{DEFAULT_SLICE_NS};
  bool pinned{false};
  bool allocate_amx{false};
  std::uint64_t initial_fs_base{0};
  std::uint64_t initial_gs_base{0};
};

/// @brief What Thread::stack_cache_stats() says.
struct StackCacheStats {
  std::uint32_t cached;  ///< Stacks kept for the next threads.
  std::uint64_t reused;  ///< Threads that got one, so far.
};

/// @brief Why a thread is leaving its CPU, for the statistics.
enum class SwitchReason : std::uint8_t {
  Voluntary,  ///< It asked to: it blocked, yielded or finished.
  Preempted,  ///< An interrupt took the CPU away.
};

/// @brief What Thread::begin_kernel_fpu() did, for Thread::end_kernel_fpu() to undo.
enum class KernelFpuSection : std::uint8_t {
  Nested,  ///< Inside another section, which takes care of everything.
  Direct,  ///< Outermost, on a kernel thread: the section works on the thread's own registers.
  Saved,   ///< Outermost. The thread's state went to its save area and comes back at the end.
};

/// @brief Concept constraining valid stateless nothrow thread functors/lambdas.
template <typename Callable>
concept StatelessThreadLambda = std::is_empty_v<std::decay_t<Callable>> &&
                                std::is_default_constructible_v<std::decay_t<Callable>> &&
                                std::is_nothrow_invocable_r_v<void, std::decay_t<Callable>>;

class Thread;
class Mutex;
class WakeList;
struct MutexWaiter;

/// @brief Returns the Thread* currently executing on the calling CPU core.
[[nodiscard]] Thread* current() noexcept;

/// @brief Moves the calling CPU from @p prev to @p next, returning once something switches back
///        to @p prev.
///
///         1) Validates @p prev and @p next stack canaries and stack pointer bounds.
///         2) Updates ns execution accounting.
///         3) Reads back what user code may have changed by itself, if @p prev is a user thread:
///            the FS and GS bases (with CR4.FSGSBASE) and PKRU.
///         4) Saves/restores FPU/SSE/AVX state and switches IA32_XFD in between.
///         5) For a user @p next: its ring-0 stack, its address space and its GS base, each only
///            if the CPU does not have it already. A kernel @p next keeps the address space it
///            finds (lazy TLB), as it only ever uses the kernel half.
///         6) Switches TLS `%fs` base and PKRU.
///         7) Updates per-cpu current thread pointer and transitions states.
///         8) Invokes `switch_context(&prev->m_rsp, next->m_rsp)`.
///
/// The stack checks stay on in release builds. They cost two loads, and a corrupted thread is
/// better stopped here than wherever its stack happens to send it.
///
/// @pre @p prev is the calling thread and outside any FpuGuard. @p next is not running anywhere.
void switch_to(Thread* prev, Thread* next, SwitchReason reason = SwitchReason::Voluntary) noexcept;

/// @brief A kernel thread, or a user thread. See the file comment.
///
/// The Entity base is what the scheduler's fair tier knows of it. The list links are for the
/// real-time run queues, for waiting to be destroyed, and for the threads of a process.
class alignas(std::hardware_destructive_interference_size) Thread
    : public Entity,
      public utils::SListNode<Thread, RunQueueTag>,
      public utils::SListNode<Thread, ReapQueueTag>,
      public utils::SListNode<Thread, ProcessThreadTag> {
 public:
  using RawEntryFn = void (*)(void* arg) noexcept;
  using ExitHookFn = void (*)(Thread* self, void* cookie) noexcept;
  using DestroyHookFn = void (*)(Thread* thread, void* cookie) noexcept;
  static constexpr std::size_t MAX_NAME_LEN{24};

  /// @brief How many kernel stacks are kept for new threads at most. See stack_cache_stats().
  static constexpr std::uint32_t STACK_CACHE_SIZE{32};

 private:
  using ErasedEntryFn = void (*)() noexcept;

  memory::VirtualAddress m_rsp{};
  memory::VirtualAddress m_stack_top{};
  memory::VirtualAddress m_stack_base{};
  std::size_t m_stack_size{0};
  std::uint64_t m_expected_canary{0};
  ThreadId m_tid{INVALID_THREAD_ID};
  std::atomic<std::uint32_t> m_flags{0};
  std::atomic<std::uint32_t> m_cpu_id{0};  ///< Where it runs or last ran. See Scheduler::wake().
  ArchitecturalState m_arch{};

  RawEntryFn m_entry{nullptr};
  ErasedEntryFn m_typed_entry{nullptr};  ///< What spawn_kernel() was given, see run_typed_entry().
  void* m_arg{nullptr};
  ExitHookFn m_on_exit{nullptr};
  void* m_exit_cookie{nullptr};
  DestroyHookFn m_on_destroy{nullptr};
  void* m_destroy_cookie{nullptr};
  proc::Process* m_process{nullptr};
  core::ipc::ThreadIpcContext* m_ipc{nullptr};  ///< See ipc_context().

  // What a user thread takes of the kernel, counted against its process's quota until it is
  // destroyed. Nothing for a kernel thread.
  memory::QuotaCharge m_charge{};

  // Where a thread made by create_user_copy() carries on, until it does. Heap memory.
  hw::user::ResumeFrame* m_resume{nullptr};
  std::uint32_t m_preferred_numa_node{NO_PREFERENCE};

  // Where a user thread starts in ring 3, and its stack there, in its process's address space.
  memory::VirtualAddress m_user_entry{};
  std::uint64_t m_user_arg{0};
  memory::VirtualAddress m_user_stack_base{};
  memory::VirtualAddress m_user_stack_top{};

  ThreadStats m_stats{};
  std::array<char, MAX_NAME_LEN> m_name{};

  std::atomic<std::uint32_t> m_retained{0};   ///< See retain().
  std::atomic<Thread*> m_wake_next{nullptr};  ///< Next on a WakeList; null if on none.

  // Priority inheritance, all of it under m_pi_lock. See mutex.hpp for how that fits in.
  utils::QSpinlock m_pi_lock{};
  Priority m_base_priority{};          ///< What the thread is by itself.
  Priority m_effective_priority{};     ///< That, or what it inherited if that is more.
  MutexWaiter* m_blocked_on{nullptr};  ///< Its place in line for a Mutex, if it is in one.
  utils::SList<Mutex, HeldMutexTag> m_contended_mutexes{};  ///< Held, and someone is in line.

  friend void switch_to(Thread* prev, Thread* next, SwitchReason reason) noexcept;
  friend class Mutex;
  friend class WakeList;

  template <typename Mutator>
    requires std::invocable<Mutator, ThreadFlags>
  void update_flags(Mutator&& fn) noexcept {
    std::uint32_t cur = m_flags.load(std::memory_order_relaxed);
    while (true) {
      const ThreadFlags next = fn(ThreadFlags{cur});
      if (m_flags.compare_exchange_weak(cur, next.raw(), std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        break;
      }
    }
  }

  // Calling a function through a pointer to another type is undefined, even one that only differs
  // in a parameter's pointee. A cast there and back is not, so this is what actually runs.
  template <typename Payload>
  static void run_typed_entry(void* payload) noexcept {
    const auto entry = reinterpret_cast<void (*)(Payload*) noexcept>(current()->m_typed_entry);
    entry(static_cast<Payload*>(payload));
  }

  void prepare_initial_stack() noexcept;
  void release_fpu_storage() noexcept;
  void release_user_stack() noexcept;
  void set_name(std::string_view name) noexcept;

  [[noreturn]] static void user_thread_start(void* self) noexcept;

  // What both kinds of user thread start out as: the kernel half, a kernel thread whose entry
  // goes to ring 3 and does not come back, charged to the tree of @p process along with
  // @p extra_charge bytes that go with it.
  [[nodiscard]] static auto create_user_half(std::string_view name, proc::Process* process,
                                             const UserThreadCreateOptions& opts,
                                             std::size_t extra_charge) noexcept
      -> std::expected<Thread*, Error>;

 public:
  Thread() noexcept = default;
  ~Thread() noexcept;

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

  /// @brief Creates the slab caches for threads and save areas.
  /// @pre The heap works and hw::fpu::initialize() ran on the BSP. Called once on the BSP, before
  ///      anything else here.
  static void initialize_subsystem() noexcept;

  /// @brief Creates a kernel thread, ready to be switched to.
  ///
  /// The stack gets a random canary in its lowest word and an unmapped gap below it, so running
  /// off the end faults, or at worst is caught at the next switch.
  ///
  /// @return The thread, or Error::InvalidArgument for a null @p entry or a stack smaller than
  ///         MIN_KSTACK_SIZE, Error::InvalidAlignment for a stack size that is not a multiple of
  ///         STACK_ALIGNMENT, Error::OutOfMemory, or Error::NotImplemented for
  ///         `opts.allocate_amx` without AMX.
  /// @note The thread does not run until it goes to Scheduler::enqueue().
  [[nodiscard]] static auto create_kernel(std::string_view name, RawEntryFn entry,
                                          void* arg = nullptr,
                                          const ThreadCreateOptions& opts = {}) noexcept
      -> std::expected<Thread*, Error>;

  /// @brief Type-safe wrapper for spawning a kernel thread with a strongly-typed payload pointer.
  template <typename Payload>
  [[nodiscard]] static auto spawn_kernel(std::string_view name, void (*entry)(Payload*) noexcept,
                                         Payload* payload,
                                         const ThreadCreateOptions& opts = {}) noexcept
      -> std::expected<Thread*, Error> {
    if (!entry) {
      return std::unexpected(Error::InvalidArgument);
    }

    auto thread = create_kernel(name, &run_typed_entry<Payload>, payload, opts);
    if (thread) {
      (*thread)->m_typed_entry = reinterpret_cast<ErasedEntryFn>(entry);
    }

    return thread;
  }

  /// @brief Zero-allocation overload for spawning a stateless lambda or functor
  template <StatelessThreadLambda Callable>
  [[nodiscard]] static auto spawn_kernel(std::string_view name, Callable&&,
                                         const ThreadCreateOptions& opts = {}) noexcept
      -> std::expected<Thread*, Error> {
    using Decayed = std::decay_t<Callable>;
    return create_kernel(name, [](void*) noexcept { Decayed{}(); }, nullptr, opts);
  }

  /// @brief Creates a user thread of @p process, which starts at @p entry in ring 3 with @p arg
  ///        in RDI and a fresh stack in the process's address space.
  ///
  /// The stack pointer starts eight bytes below the top of that stack, where a call would have
  /// left a return address, so @p entry can be an ordinary function. That return address is
  /// zero: a function that returns ends up at address zero, and its process with it.
  ///
  /// This only makes the thread. A process links it into its own list and hands it to the
  /// scheduler (see proc::Process::spawn_thread()). What the kernel holds for it, the thread, its
  /// kernel stack and FPU state, counts against the process's quota until it is destroyed, and its
  /// stack against the address space's.
  /// @return The thread, or Error::InvalidArgument for a process without an address space or an
  ///         @p entry user code cannot have, Error::QuotaExceeded, Error::OutOfMemory, and
  ///         whatever create_kernel() says about the kernel side.
  [[nodiscard]] static auto create_user(std::string_view name, proc::Process* process,
                                        memory::VirtualAddress entry, std::uint64_t arg = 0,
                                        const UserThreadCreateOptions& opts = {}) noexcept
      -> std::expected<Thread*, Error>;

  /// @brief A user thread of @p process that carries on from where the calling thread is in user
  ///        code, at @p at, rather than from an entry point: what a fork() that returns in the
  ///        child too makes.
  ///
  /// Everything else the CPU holds for the calling thread goes along as it is right now: its FPU
  /// and vector registers, its FS and GS bases and PKRU. The process's address space is meant to
  /// be a copy of the caller's, so the stack @p at points into is there too, and the new thread
  /// takes over the copy of the caller's stack instead of getting one of its own. It has the
  /// caller's priority, and AMX if the caller has it. Like create_user(), this only makes it.
  /// @pre The calling thread is a user thread, outside any kernel FPU section.
  /// @return The thread, or Error::InvalidArgument for a process without an address space, or a
  ///         place or stack pointer user code cannot have, Error::InvalidState if the caller is
  ///         not a user thread, Error::QuotaExceeded, or Error::OutOfMemory.
  [[nodiscard]] static auto create_user_copy(std::string_view name, proc::Process* process,
                                             const hw::user::ResumeFrame& at) noexcept
      -> std::expected<Thread*, Error>;

  /// @brief How many kernel stacks are kept for new threads, and how often one was taken.
  ///
  /// Stacks of the default size do not go back to the VMM when their thread is destroyed, as long
  /// as there is room: the next thread takes one that is mapped already, and nothing has to be
  /// unmapped or shot down from the TLBs. Under memory pressure they go back after all (see
  /// memory/shrinker.hpp).
  [[nodiscard]] static StackCacheStats stack_cache_stats() noexcept;

  /// @brief Wraps the calling CPU's boot stack into a live `Running` idle thread, and makes it the
  ///        CPU's current one.
  /// @param cpu_id     The calling CPU.
  /// @param stack_base Lowest address of the stack, where its canary goes. Not the top: that is
  ///                   `stack_base + stack_size`.
  /// @param stack_size Size of the stack, at least MIN_KSTACK_SIZE.
  /// @return The thread, or Error::InvalidArgument, Error::InvalidState if the CPU already has
  ///         one, or Error::OutOfMemory.
  [[nodiscard]] static auto adopt_boot_thread(std::uint32_t cpu_id,
                                              memory::VirtualAddress stack_base,
                                              std::size_t stack_size,
                                              std::string_view name = "idle") noexcept
      -> std::expected<Thread*, Error>;

  /// @brief Destroys a `Dead` or never-started thread, releasing its kernel stack (if not
  ///        borrowed), its user stack, FPU save area, and TCB slab object.
  ///
  /// `Dead` alone does not mean that a thread has left its stack: it is still on it while its exit
  /// hook runs. Whoever calls this has to know it is gone. The destroy hook runs last, with the
  /// thread still there to look at but no longer able to run.
  ///
  /// @return Error::InvalidArgument for null, Error::InvalidState for the calling thread, one that
  ///         started and has not finished, or one the scheduler has and that is not dead yet.
  [[nodiscard]] static auto destroy(Thread* thread) noexcept -> std::expected<void, Error>;

  /// @brief Gives this thread a save area in the initial FPU state, if it has none yet.
  [[nodiscard]] auto enable_fpu_context() noexcept -> std::expected<void, Error>;

  /// @brief Makes this thread an AMX user: a save area with room for tile data, keeping what
  ///        it has so far, and the tile registers enabled whenever it runs. A user thread pays
  ///        for the larger area out of its process's quota.
  /// @return Error::NotImplemented if the CPU has no AMX, Error::QuotaExceeded, or
  ///         Error::OutOfMemory.
  [[nodiscard]] auto request_amx_context() noexcept -> std::expected<void, Error>;

  /// @brief Starts a section of kernel code using the FPU. Use utils::FpuGuard instead.
  ///
  /// A kernel thread only ever touches the FPU in these sections, so its registers are simply
  /// its own, what one section leaves is there for the next, and none of this costs an XSAVE. The
  /// registers of a user thread are the user's: those go to the save area here, and come back at
  /// the end.
  ///
  /// @pre Preemption is off and the CPU is not in an interrupt handler, which would have no way of
  ///      knowing what it was about to trample. Only the calling thread may do this.
  [[nodiscard]] KernelFpuSection begin_kernel_fpu() noexcept;

  /// @brief Ends the section @p section came from.
  void end_kernel_fpu(KernelFpuSection section) noexcept;

  [[nodiscard]] constexpr ThreadId tid() const noexcept { return m_tid; }
  [[nodiscard]] constexpr std::string_view name() const noexcept {
    return std::string_view{m_name.data()};
  }

  [[nodiscard]] ThreadFlags flags() const noexcept {
    return ThreadFlags{m_flags.load(std::memory_order_acquire)};
  }

  [[nodiscard]] ThreadState state() const noexcept { return flags().get_state(); }
  void set_state(ThreadState new_state) noexcept {
    update_flags([new_state](ThreadFlags f) { return f.with_state(new_state); });
  }

  /// @brief The class the thread competes in, which may be one it inherited.
  [[nodiscard]] PriorityClass priority_class() const noexcept {
    return flags().get_priority_class();
  }

  /// @brief What the thread is by itself, whatever it may have inherited.
  [[nodiscard]] constexpr Priority base_priority() const noexcept { return m_base_priority; }

  /// @brief Moves the thread to another priority class, and to the weight that goes with it.
  /// @pre The thread is neither running nor on a run queue, and nobody waits for a Mutex it holds.
  void set_priority_class(PriorityClass prio) noexcept {
    m_base_priority = Priority::of(prio);
    m_effective_priority = m_base_priority;
    adopt_priority(m_base_priority);
  }

  /// @brief Competes with @p priority from now on. RunQueue::set_priority_locked() does this for a
  ///        thread that is on a queue.
  /// @pre The thread is on no queue. It goes by the lock of the queue it last ran on.
  void adopt_priority(const Priority priority) noexcept {
    adopt_priority_class(priority.priority_class);
    set_weight(priority.weight);
  }

  /// @brief Just the class, for a thread whose weight is being taken care of.
  void adopt_priority_class(const PriorityClass prio) noexcept {
    update_flags([prio](ThreadFlags f) { return f.with_priority_class(prio); });
  }

  [[nodiscard]] bool is_kthread() const noexcept { return flags().get_is_kthread(); }
  [[nodiscard]] bool is_idle() const noexcept { return flags().get_is_idle(); }
  [[nodiscard]] bool is_pinned() const noexcept { return flags().get_is_pinned(); }
  [[nodiscard]] bool uses_fpu() const noexcept { return flags().get_uses_fpu(); }
  [[nodiscard]] bool uses_amx() const noexcept { return flags().get_uses_amx(); }
  [[nodiscard]] bool in_kernel_fpu() const noexcept { return flags().get_in_kernel_fpu(); }
  [[nodiscard]] bool on_runqueue() const noexcept { return flags().get_on_runqueue(); }
  [[nodiscard]] bool exit_requested() const noexcept { return flags().get_exit_requested(); }
  [[nodiscard]] bool migrating() const noexcept { return flags().get_migrating(); }

  /// @brief Whether someone is in line for a Mutex this thread holds. A rough look for assertions.
  [[nodiscard]] bool holds_contended_mutex() const noexcept { return !m_contended_mutexes.empty(); }

  void set_on_runqueue(bool on_rq) noexcept {
    update_flags([on_rq](ThreadFlags f) { return f.with_on_runqueue(on_rq); });
  }

  /// @brief Records that the thread is waiting on a run queue, in one atomic step.
  void mark_queued() noexcept {
    update_flags([](ThreadFlags f) {
      return f.with_on_runqueue().without_migrating().with_state(ThreadState::Ready);
    });
  }

  /// @brief Records that the thread is runnable and on its way to a run queue.
  ///
  /// With only one queue locked at a time, there is a moment when it is on none. Nobody may touch
  /// what a queue relies on until it arrives, which is what Scheduler::apply_priority() waits for.
  void mark_migrating() noexcept {
    update_flags([](ThreadFlags f) {
      return f.with_migrating().without_on_runqueue().with_state(ThreadState::Ready);
    });
  }

  /// @brief Ends a block or a sleep, as one step, and only if the thread is still in one.
  ///
  /// A thread that is running changes its own state without asking anyone. Looking first and
  /// writing afterwards would let it get on with things in between, all the way to being `Dead`,
  /// and then bring it back to life.
  /// @param next Running, for a thread that never left its CPU. Ready and on its way to a queue,
  ///             with Thread::mark_migrating(), for one that did.
  /// @return false if it was neither blocked nor asleep.
  [[nodiscard]] bool try_unblock(const ThreadState next) noexcept {
    std::uint32_t cur = m_flags.load(std::memory_order_relaxed);

    while (true) {
      const ThreadFlags flags{cur};
      const ThreadState state = flags.get_state();
      if (state != ThreadState::Blocked && state != ThreadState::Sleeping) {
        return false;
      }

      const ThreadFlags wanted =
          next == ThreadState::Running
              ? flags.with_state(ThreadState::Running)
              : flags.with_migrating().without_on_runqueue().with_state(ThreadState::Ready);

      if (m_flags.compare_exchange_weak(cur, wanted.raw(), std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        return true;
      }
    }
  }

  /// @brief Leaves a mark for the thread to find in its next sleep, which may be the current one.
  void request_sleep_cancel() noexcept {
    update_flags([](ThreadFlags f) { return f.with_sleep_cancelled(); });
  }

  /// @brief Takes the mark request_sleep_cancel() left.
  /// @return Whether there was one.
  [[nodiscard]] bool take_sleep_cancel() noexcept {
    const std::uint32_t mask = ThreadFlags{}.with_sleep_cancelled().raw();
    return (m_flags.fetch_and(~mask, std::memory_order_acq_rel) & mask) != 0;
  }

  /// @brief Keeps the thread's memory around until release().
  ///
  /// Whoever found a thread under a lock can go on using it without the lock this way. A wakeup
  /// needs that: once off the queue, the thread can finish waiting for another reason and be
  /// destroyed before the wakeup gets to it. Thread::destroy() waits for this to end, so it is only
  /// for code that cannot be preempted in the meantime.
  void retain() noexcept { m_retained.fetch_add(1, std::memory_order_relaxed); }

  void release() noexcept { m_retained.fetch_sub(1, std::memory_order_release); }

  /// @brief Hands the thread to the scheduler, which only works once.
  ///
  /// A flag of its own, because nothing else can tell: a thread another CPU just picked is on no
  /// queue, and not running yet either, for as long as it takes that CPU to get to switch_to().
  /// @return false if something else got there first.
  [[nodiscard]] bool try_hand_to_scheduler() noexcept {
    std::uint32_t cur = m_flags.load(std::memory_order_relaxed);
    do {
      if (ThreadFlags{cur}.get_scheduled()) {
        return false;
      }
    } while (!m_flags.compare_exchange_weak(cur, ThreadFlags{cur}.with_scheduled().raw(),
                                            std::memory_order_acq_rel, std::memory_order_relaxed));

    return true;
  }

  /// @brief Takes back try_hand_to_scheduler(), for a thread the scheduler had to turn down.
  void revoke_from_scheduler() noexcept {
    update_flags([](ThreadFlags f) { return f.without_scheduled(); });
  }

  void request_exit() noexcept {
    update_flags([](ThreadFlags f) { return f.with_exit_requested(); });
  }

  /// @brief Sets what runs, on this thread, after its entry function returned.
  void set_exit_hook(ExitHookFn hook, void* cookie = nullptr) noexcept {
    m_on_exit = hook;
    m_exit_cookie = cookie;
  }

  /// @brief Sets what runs when the thread is destroyed, on whatever destroys it.
  ///
  /// The thread is gone from every CPU by then and its stacks are released, so this is the place
  /// for whoever keeps track of it to let go. A process counts its threads this way.
  void set_destroy_hook(DestroyHookFn hook, void* cookie = nullptr) noexcept {
    m_on_destroy = hook;
    m_destroy_cookie = cookie;
  }

  /// @brief The destroy hook set so far, for whoever sets one of its own and calls this one on.
  [[nodiscard]] DestroyHookFn destroy_hook() const noexcept { return m_on_destroy; }
  [[nodiscard]] void* destroy_cookie() const noexcept { return m_destroy_cookie; }

  /// @brief The thread's side of IPC, if it has one (see core/ipc/endpoint.hpp). Set before the
  ///        thread runs, by whoever gives it one, and only ever used by the thread itself.
  [[nodiscard]] core::ipc::ThreadIpcContext* ipc_context() const noexcept { return m_ipc; }
  void set_ipc_context(core::ipc::ThreadIpcContext* ctx) noexcept { m_ipc = ctx; }

  [[nodiscard]] constexpr memory::VirtualAddress saved_rsp() const noexcept { return m_rsp; }
  [[nodiscard]] constexpr memory::VirtualAddress* saved_rsp_ptr() noexcept { return &m_rsp; }
  [[nodiscard]] constexpr memory::VirtualAddress stack_base() const noexcept {
    return m_stack_base;
  }

  [[nodiscard]] constexpr memory::VirtualAddress stack_top() const noexcept { return m_stack_top; }
  [[nodiscard]] constexpr std::size_t stack_size() const noexcept { return m_stack_size; }

  [[nodiscard]] std::uint32_t cpu_id() const noexcept {
    return m_cpu_id.load(std::memory_order_acquire);
  }

  /// @brief Says which run queue the thread belongs to from now on.
  ///
  /// Only under the lock of the queue it belongs to so far, or before the scheduler has it. That
  /// is the rule that makes "lock the queue cpu_id() names, then look again" mean anything: as long
  /// as the lock is held, the thread cannot be anywhere else, and nothing can take it away. Waking
  /// a thread or stealing it decides where it goes next before letting go of that lock.
  void set_cpu_id(const std::uint32_t cpu) noexcept {
    m_cpu_id.store(cpu, std::memory_order_release);
  }

  [[nodiscard]] constexpr std::uint32_t preferred_numa_node() const noexcept {
    return m_preferred_numa_node;
  }

  constexpr void set_preferred_numa_node(std::uint32_t node) noexcept {
    m_preferred_numa_node = node;
  }

  [[nodiscard]] constexpr proc::Process* process() const noexcept { return m_process; }
  constexpr void set_process(proc::Process* proc) noexcept { m_process = proc; }

  /// @brief Whether the thread runs user code: one of a process, which goes to ring 3.
  [[nodiscard]] bool is_user() const noexcept { return !is_kthread(); }

  [[nodiscard]] constexpr memory::VirtualAddress user_entry() const noexcept {
    return m_user_entry;
  }

  /// @brief The lowest address of the user stack, as it was made: it can grow below this.
  [[nodiscard]] constexpr memory::VirtualAddress user_stack_base() const noexcept {
    return m_user_stack_base;
  }

  [[nodiscard]] constexpr memory::VirtualAddress user_stack_top() const noexcept {
    return m_user_stack_top;
  }

  [[nodiscard]] constexpr ArchitecturalState& arch_state() noexcept { return m_arch; }
  [[nodiscard]] constexpr const ArchitecturalState& arch_state() const noexcept { return m_arch; }

  [[nodiscard]] constexpr ThreadStats& stats() noexcept { return m_stats; }
  [[nodiscard]] constexpr const ThreadStats& stats() const noexcept { return m_stats; }

  /// @brief Checks whether the 64-bit cryptographic canary at the base of the thread's
  ///        kernel stack matches the expected value generated at creation.
  [[nodiscard]] bool verify_stack_canary() const noexcept {
    if (m_stack_base.value() == 0) {
      return false;
    }

    const auto* canary_slot = m_stack_base.as<const volatile std::uint64_t>();
    return *canary_slot == m_expected_canary;
  }

  /// @brief Verifies that a stack pointer lies strictly within `[m_stack_base + 8, m_stack_top]`
  ///        and satisfies 8-byte alignment.
  [[nodiscard]] constexpr bool is_stack_pointer_valid(memory::VirtualAddress sp) const noexcept {
    const std::uint64_t addr = sp.value();
    const std::uint64_t low = m_stack_base.value() + sizeof(std::uint64_t);
    const std::uint64_t high = m_stack_top.value();
    return (addr >= low) && (addr <= high) && ((addr & 0x7ul) == 0);
  }

  /// @brief Returns the number of bytes currently in use on this thread's stack
  ///        based on its saved `m_rsp`.
  [[nodiscard]] constexpr std::size_t stack_usage_bytes() const noexcept {
    if (m_rsp.value() == 0 || m_rsp.value() > m_stack_top.value()) {
      return 0;
    }

    return (m_stack_top.value() - m_rsp.value());
  }

  /// @brief Returns the remaining free stack bytes above the stack canary slot.
  [[nodiscard]] constexpr std::size_t stack_free_bytes() const noexcept {
    const std::size_t used = stack_usage_bytes();
    const std::size_t usable =
        m_stack_size > sizeof(std::uint64_t) ? (m_stack_size - sizeof(std::uint64_t)) : 0;
    return (used < usable) ? (usable - used) : 0;
  }

  /// @brief Returns a pointer to the synthesized or saved `SwitchContext` at `m_rsp`.
  [[nodiscard]] const SwitchContext* inspect_switch_context() const noexcept {
    if (!is_stack_pointer_valid(m_rsp)) {
      return nullptr;
    }

    return m_rsp.as<const SwitchContext>();
  }

  /// @brief Invokes the thread's entry payload and transitions to `Dead` upon completion.
  ///
  /// Only ever reached from assembly, which is what keeps LTO from seeing a caller.
  [[noreturn, gnu::used]] static void entry_launchpad(Thread* self) noexcept
      asm("sched_entry_launchpad");
};

static_assert(alignof(Thread) == std::hardware_destructive_interference_size);

/// @brief Naked Assembly Context Switch Routine.
///
/// Saves callee-saved registers onto the current stack, stores `%rsp` into a dereferenced @p
/// prev_rsp, loads `%rsp` from @p next_rsp, restores callee-saved registers, and returns into @p
/// next's saved `rip`.
[[gnu::naked, gnu::noinline]] void switch_context(std::uint64_t* prev_rsp,
                                                  std::uint64_t next_rsp) noexcept;
}  // namespace kernel::sched
