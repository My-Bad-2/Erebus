/// @file thread.hpp
/// @brief Kernel threads, and switching between them.
///
/// A Thread is a kernel stack plus whatever else the CPU needs to carry on where it left off.
/// There is no scheduler yet, so this file is only the mechanism: switch_to() moves a CPU from
/// one thread to another, and deciding when is up to the caller.
///
/// Every CPU gets a Thread around the stack it booted on before it goes online, so current()
/// is never null from then on. Everything else comes from create_kernel(), starts out `Ready`,
/// and first runs when something switches to it. When its entry function returns it turns
/// `Dead` and calls its exit hook, which is the place to switch away for good. Nothing can leave
/// the CPU otherwise, since there is nobody to hand it to yet.
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
#include "utils/linked_list.hpp"

namespace kernel::proc {
class Process;
}

namespace kernel::sched {
using ThreadId = std::uint64_t;
inline constexpr ThreadId INVALID_THREAD_ID{0};

inline constexpr std::size_t DEFAULT_KSTACK_SIZE = 32 * 1024;  // 32 KiB
inline constexpr std::size_t MIN_KSTACK_SIZE = 16 * 1024;      // 16 KiB
inline constexpr std::size_t STACK_ALIGNMENT = 16;

/// @brief Thread execution lifecycle states.
enum class ThreadState : std::uint8_t {
  Created = 0,   ///< Allocated and initialized, but not yet enqueued or switched in
  Ready = 1,     ///< Runnable and waiting to be scheduled onto a CPU
  Running = 2,   ///< Currently executing on a CPU Core
  Blocked = 3,   ///< Waiting on a synchronization primitive
  Sleeping = 4,  ///< Waiting for a monotonic timer deadline
  Dead = 5,      ///< Finished execution. Awaiting stack/TCB reclamation
};

/// @brief Scheduling priority classes used to parameterize thread weight and latency targets
enum class PriorityClass : std::uint8_t {
  Idle = 0,        ///< Per-CPU idle thread
  Background = 1,  ///< Low-Priority background maintenance / compaction daemons
  Normal = 2,      ///< Standard Kernel and User Threads (EEVDF baseline weight = 1024)
  High = 3,        ///< Latency-sensitive kernel worker / bottom-half threads
  RealTime = 4,    ///< Fixed-priority / immediate preemption threads
  TestRunner = 5,  ///< Pinned in-kernel ktest runner threads
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
///   [13]      : on_runqueue (1 = queued on a scheduler runqueue)
///   [14]      : stack_borrowed (1 = uses pre-allocated boot stack; do not vmm->free on exit)
///   [15]      : exit_requested (1 = cooperative cancellation / stop signal pending)
///   [16..23]  : nice_bias (signed 8-bit nice offset encoded as biased [0..39])
///   [24]      : uses_amx (1 = thread explicitly opted into 8 KiB of Intel AMX tile data)
///   [25]      : in_kernel_fpu (1 = thread is currently inside a Ring-0 FPU guard scope)
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
  std::uint32_t preferred_cpu{0};
  std::uint32_t preferred_numa_node{0};
  bool pinned{false};
  bool allocate_fpu{false};  ///< Gives the thread a save area from the start
  bool allocate_amx{false};  ///< The same with room for Intel AMX tile data; fails without AMX
  std::uint64_t initial_fs_base{0};
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

/// @brief Returns the Thread* currently executing on the calling CPU core.
[[nodiscard]] Thread* current() noexcept;

/// @brief Moves the calling CPU from @p prev to @p next, returning once something switches back
///        to @p prev.
///
///         1) Validates @p prev and @p next stack canaries and stack pointer bounds.
///         2) Updates ns execution accounting.
///         3) Saves/restores FPU/SSE/AVX state and switches IA32_XFD in between.
///         4) Updates Ring-0 privilege stack.
///         5) Switches TLS `%fs` base and PKRU.
///         6) Updates per-cpu current thread pointer and transitions states.
///         7) Invokes `switch_context(&prev->m_rsp, next->m_rsp)`.
///
/// The stack checks stay on in release builds. They cost two loads, and a corrupted thread is
/// better stopped here than wherever its stack happens to send it.
///
/// @pre @p prev is the calling thread and outside any FpuGuard. @p next is not running anywhere.
void switch_to(Thread* prev, Thread* next) noexcept;

/// @brief A kernel thread. See the file comment. The list link is for run and wait queues.
class alignas(std::hardware_destructive_interference_size) Thread
    : public utils::SListNode<Thread> {
 public:
  using RawEntryFn = void (*)(void* arg) noexcept;
  using ExitHookFn = void (*)(Thread* self, void* cookie) noexcept;
  static constexpr std::size_t MAX_NAME_LEN{24};

 private:
  using ErasedEntryFn = void (*)() noexcept;

  memory::VirtualAddress m_rsp{};
  memory::VirtualAddress m_stack_top{};
  memory::VirtualAddress m_stack_base{};
  std::size_t m_stack_size{0};
  std::uint64_t m_expected_canary{0};
  ThreadId m_tid{INVALID_THREAD_ID};
  std::atomic<std::uint32_t> m_flags{0};
  std::uint32_t m_cpu_id{0};
  ArchitecturalState m_arch{};

  RawEntryFn m_entry{nullptr};
  ErasedEntryFn m_typed_entry{nullptr};  ///< What spawn_kernel() was given, see run_typed_entry().
  void* m_arg{nullptr};
  ExitHookFn m_on_exit{nullptr};
  void* m_exit_cookie{nullptr};
  proc::Process* m_process{nullptr};
  std::uint32_t m_preferred_numa_node{0};

  // For the scheduler, once there is one.
  [[maybe_unused]] std::uint32_t m_weight{1024};
  [[maybe_unused]] std::uint64_t m_vruntime{0};
  [[maybe_unused]] std::uint64_t m_vdeadline{0};

  ThreadStats m_stats{};
  std::array<char, MAX_NAME_LEN> m_name{};

  friend void switch_to(Thread* prev, Thread* next) noexcept;

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
  void set_name(std::string_view name) noexcept;

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
  ///        borrowed), FPU save area, and TCB slab object.
  ///
  /// `Dead` alone does not mean that a thread has left its stack: it is still on it while its exit
  /// hook runs. Whoever calls this has to know it is gone.
  ///
  /// @return Error::InvalidArgument for null, Error::InvalidState for the calling thread or one
  ///         that started and has not finished.
  [[nodiscard]] static auto destroy(Thread* thread) noexcept -> std::expected<void, Error>;

  /// @brief Gives this thread a save area in the initial FPU state, if it has none yet.
  [[nodiscard]] auto enable_fpu_context() noexcept -> std::expected<void, Error>;

  /// @brief Makes this thread an AMX user: a save area with room for tile data, keeping what
  ///        it has so far, and the tile registers enabled whenever it runs.
  /// @return Error::NotImplemented if the CPU has no AMX, or Error::OutOfMemory.
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

  [[nodiscard]] PriorityClass priority_class() const noexcept {
    return flags().get_priority_class();
  }
  void set_priority_class(PriorityClass prio) noexcept {
    update_flags([prio](ThreadFlags f) { return f.with_priority_class(prio); });
  }

  [[nodiscard]] bool is_kthread() const noexcept { return flags().get_is_kthread(); }
  [[nodiscard]] bool is_idle() const noexcept { return flags().get_is_idle(); }
  [[nodiscard]] bool is_pinned() const noexcept { return flags().get_is_pinned(); }
  [[nodiscard]] bool uses_fpu() const noexcept { return flags().get_uses_fpu(); }
  [[nodiscard]] bool uses_amx() const noexcept { return flags().get_uses_amx(); }
  [[nodiscard]] bool in_kernel_fpu() const noexcept { return flags().get_in_kernel_fpu(); }
  [[nodiscard]] bool exit_requested() const noexcept { return flags().get_exit_requested(); }

  void request_exit() noexcept {
    update_flags([](ThreadFlags f) { return f.with_exit_requested(); });
  }

  /// @brief Sets what runs, on this thread, after its entry function returned.
  void set_exit_hook(ExitHookFn hook, void* cookie = nullptr) noexcept {
    m_on_exit = hook;
    m_exit_cookie = cookie;
  }

  [[nodiscard]] constexpr memory::VirtualAddress saved_rsp() const noexcept { return m_rsp; }
  [[nodiscard]] constexpr memory::VirtualAddress* saved_rsp_ptr() noexcept { return &m_rsp; }
  [[nodiscard]] constexpr memory::VirtualAddress stack_base() const noexcept {
    return m_stack_base;
  }

  [[nodiscard]] constexpr memory::VirtualAddress stack_top() const noexcept { return m_stack_top; }
  [[nodiscard]] constexpr std::size_t stack_size() const noexcept { return m_stack_size; }

  [[nodiscard]] constexpr std::uint32_t cpu_id() const noexcept { return m_cpu_id; }
  constexpr void set_cpu_id(std::uint32_t cpu) noexcept { m_cpu_id = cpu; }

  [[nodiscard]] constexpr std::uint32_t preferred_numa_node() const noexcept {
    return m_preferred_numa_node;
  }

  constexpr void set_preferred_numa_node(std::uint32_t node) noexcept {
    m_preferred_numa_node = node;
  }

  [[nodiscard]] constexpr proc::Process* process() const noexcept { return m_process; }
  constexpr void set_process(proc::Process* proc) noexcept { m_process = proc; }

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
