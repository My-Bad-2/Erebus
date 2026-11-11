#include "sched/thread.hpp"

#include <memory>
#include <span>

#include "crypto/random.hpp"
#include "drivers/clock.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/user.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/shrinker.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "proc/process.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/maths.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::sched {
namespace {
std::atomic<ThreadId> g_next_tid{1};

memory::heap::KmemCache* g_thread_cache{nullptr};
memory::heap::KmemCache* g_fpu_cache{nullptr};
memory::heap::KmemCache* g_amx_fpu_cache{nullptr};

class StackCache {
 public:
  static constexpr std::uint32_t CAPACITY = Thread::STACK_CACHE_SIZE;
  static constexpr std::size_t STACK_PAGES = DEFAULT_KSTACK_SIZE / memory::PAGE_SIZE;

  /// @return false if it is full, in which case the stack is still the caller's.
  [[nodiscard]] bool put(const memory::VirtualAddress base) noexcept {
    const std::uint32_t node = node_of(base);

    utils::IrqSaveGuard guard{m_lock};
    if (m_count == CAPACITY) {
      return false;
    }

    m_entries[m_count++] = {.base = base, .node = node};
    return true;
  }

  /// @return A stack, from the caller's node if there is one, or null.
  [[nodiscard]] memory::VirtualAddress take() noexcept {
    utils::IrqSaveGuard guard{m_lock};
    if (m_count == 0) {
      return {};
    }

    std::uint32_t pick = m_count - 1;
    const std::uint32_t home = hw::percpu::numa_node_id();
    for (std::uint32_t i = m_count; i-- > 0;) {
      if (m_entries[i].node == home) {
        pick = i;
        break;
      }
    }

    const memory::VirtualAddress base = m_entries[pick].base;
    m_entries[pick] = m_entries[--m_count];
    m_reused.fetch_add(1, std::memory_order_relaxed);
    return base;
  }

  /// @brief Gives the stacks of @p node back to the VMM. @return How many pages that was.
  std::size_t release(const std::uint32_t node) noexcept {
    std::array<memory::VirtualAddress, CAPACITY> going{};
    std::uint32_t count = 0;
    {
      utils::IrqSaveGuard guard{m_lock};
      for (std::uint32_t i = 0; i < m_count;) {
        if (m_entries[i].node == node) {
          going[count++] = m_entries[i].base;
          m_entries[i] = m_entries[--m_count];
        } else {
          ++i;
        }
      }
    }

    // Unmapping waits for the other CPUs, which is not for under a spinlock.
    for (std::uint32_t i = 0; i < count; ++i) {
      if (const auto freed = memory::vmm::kernel_space()->free(going[i]); !freed) [[unlikely]] {
        utils::logger::warn("sched: leaking a cached stack: {:#x}\n",
                            std::to_underlying(freed.error()));
      }
    }

    return count * STACK_PAGES;
  }

  [[nodiscard]] StackCacheStats stats() noexcept {
    utils::IrqSaveGuard guard{m_lock};
    return {.cached = m_count, .reused = m_reused.load(std::memory_order_relaxed)};
  }

 private:
  struct Entry {
    memory::VirtualAddress base;
    std::uint32_t node;
  };

  // Where the memory of a stack came from, for a shrink of that node to find it.
  [[nodiscard]] static std::uint32_t node_of(const memory::VirtualAddress base) noexcept {
    const auto found = memory::vmm::kernel_space()->pagemap().translate(base);
    return found ? memory::pmm::phys_to_page(found->phys_address)->get_numa_node() : 0;
  }

  utils::QSpinlock m_lock{};
  std::array<Entry, CAPACITY> m_entries{};
  std::uint32_t m_count{0};
  std::atomic<std::uint64_t> m_reused{0};
};

constinit StackCache g_stacks{};

std::size_t shrink_stacks(const std::uint32_t node_id, void*) noexcept {
  return g_stacks.release(node_id);
}

constinit memory::Shrinker g_stack_shrinker{&shrink_stacks, nullptr};

[[nodiscard]] memory::heap::KmemCache* create_cache(const std::size_t size,
                                                    const std::size_t alignment) noexcept {
  const auto cache = memory::heap::KmemCache::create(static_cast<std::uint32_t>(size),
                                                     static_cast<std::uint32_t>(alignment));
  if (!cache) {
    utils::logger::fatal("sched: cannot create a {}-byte object cache: {:#x}\n", size,
                         std::to_underlying(cache.error()));
  }

  return *cache;
}

[[nodiscard]] Thread* allocate_thread() noexcept {
  KASSERT(g_thread_cache != nullptr);
  const auto raw = g_thread_cache->alloc();
  return raw ? std::construct_at(static_cast<Thread*>(*raw)) : nullptr;
}

void free_thread(Thread* thread) noexcept {
  std::destroy_at(thread);
  memory::heap::kfree(thread);
}

[[nodiscard]] std::uint64_t generate_stack_canary() noexcept {
  // A low byte that is not zero: a string overrun should not get to write a valid canary.
  return (crypto::random::get<std::uint64_t>() & ~0xfful) | 0xA5ul;
}

// Whether the registers hold anything the thread's save area does not. A kernel thread only
// touches them inside FpuGuards, which keep track of that, but who knows what a user thread did.
[[nodiscard]] bool has_unsaved_fpu_state(const ThreadFlags flags) noexcept {
  return flags.get_uses_fpu() && flags.get_state() != ThreadState::Dead &&
         (!flags.get_is_kthread() || flags.get_fpu_dirty());
}

// Where a new thread's switch_context() returns to, with `r12` holding `Thread* self`. The stack
// is aligned here because the ABI wants it aligned before a call.
[[noreturn, gnu::naked]] void thread_entry_trampoline() noexcept {
  asm volatile(
      "xorl %ebp, %ebp\n\t"
      "andq $-16, %rsp\n\t"
      "movq %r12, %rdi\n\t"
      "callq sched_entry_launchpad\n\t"
      "ud2\n\t");
}
}  // namespace

namespace details {
// Interrupts from ring 3 find the stack in the TSS, and SYSCALL finds it in the CPU's area.
void update_ring0_stack_legacy(const memory::VirtualAddress stack_top) noexcept {
  hw::PerCpu* cpu = hw::percpu::self();
  cpu->gdt_table.set_kernel_stack(stack_top);
  cpu->syscall_stack = stack_top.value();
}

// FRED delivers SYSCALL like every other event, on the stack in its RSP0.
void update_ring0_stack_fred(const memory::VirtualAddress stack_top) noexcept {
  hw::percpu::self()->gdt_table.set_kernel_stack(stack_top);
  constexpr std::uint32_t MSR_FRED_RSP0 = 0x1CC;
  hw::write::msr(MSR_FRED_RSP0, stack_top.value());
}

void switch_xfd_msr(const hw::fpu::Ia32Xfd xfd) noexcept {
  hw::write::msr(hw::fpu::MSR_IA32_XFD, xfd.raw());
}

void switch_pkru(const std::uint32_t pkru) noexcept {
  asm volatile(
      "xorl %%ecx, %%ecx\n\t"
      "xorl %%edx, %%edx\n\t"
      "wrpkru"
      : /* No Output */
      : "a"(pkru)
      : "ecx", "edx", "memory");
}
}  // namespace details

DECLARE_STATIC_CALL(void, update_ring0_stack, memory::VirtualAddress);
DEFINE_STATIC_CALL(update_ring0_stack, details::update_ring0_stack_legacy);
REGISTER_STATIC_CALL_ALT(update_ring0_stack, details::update_ring0_stack_fred, hw::Feature::FRED);

DECLARE_STATIC_CALL(void, switch_xfd, hw::fpu::Ia32Xfd);
DEFINE_STATIC_CALL_NOP(switch_xfd);
REGISTER_STATIC_CALL_ALT(switch_xfd, details::switch_xfd_msr, hw::Feature::XFD);

DECLARE_STATIC_CALL(void, switch_pkru, std::uint32_t);
DEFINE_STATIC_CALL_NOP(switch_pkru);
REGISTER_STATIC_CALL_ALT(switch_pkru, details::switch_pkru, hw::Feature::PKU);

namespace {
// User code changes some of its state without the kernel hearing of it, so until a switch reads
// it back, what the CPU holds is the only copy.
void save_user_state(ArchitecturalState& arch) noexcept {
  if (hw::user::can_change_bases()) {
    arch.fs_base = hw::read::fs_base();
    arch.user_gs_base = hw::user::read_gs_base();
    hw::percpu::self()->user_gs_base = arch.user_gs_base;
  }

  arch.pkru = hw::user::read_pkru();
}

// What only a user thread needs from the CPU. Kernel threads run on the kernel half of whatever
// address space was loaded before them and never come in from ring 3, so a CPU that goes from a
// user thread to a kernel thread and back to the same user thread changes none of this.
void load_user_context(const Thread& next) noexcept {
  hw::PerCpu* cpu = hw::percpu::self();

  if (cpu->gdt_table.kernel_stack() != next.stack_top()) {
    update_ring0_stack(next.stack_top());
  }

  memory::vmm::PageMap* map = &next.process()->address_space()->pagemap();
  if (cpu->pcid_manager.active() != map) {
    cpu->pcid_manager.load(map);
  }

  const std::uint64_t gs_base = next.arch_state().user_gs_base;
  if (cpu->user_gs_base != gs_base) {
    hw::user::write_gs_base(gs_base);
    cpu->user_gs_base = gs_base;
  }
}
}  // namespace

void switch_context(std::uint64_t* prev_rsp, std::uint64_t next_rsp) noexcept {
  asm volatile(
      // Push callee-saved registers in reverse field order so `r15` sits at 0(%rsp)
      "pushq %rbp\n\t"
      "pushq %rbx\n\t"
      "pushq %r12\n\t"
      "pushq %r13\n\t"
      "pushq %r14\n\t"
      "pushq %r15\n\t"

      // Save outgoing stack pointer: *prev_rsp(%rdi) = %rsp
      "movq %rsp, (%rdi)\n\t"

      // Load incoming stack pointer: %rsp = next_rsp (%rsi)
      "movq %rsi, %rsp\n\t"

      // Restore incoming thread's callee-saved registers
      "popq %r15\n\t"
      "popq %r14\n\t"
      "popq %r13\n\t"
      "popq %r12\n\t"
      "popq %rbx\n\t"
      "popq %rbp\n\t"

      // Pop saved `rip` and transfer control to incoming thread
      "retq\n\t");
}

void Thread::initialize_subsystem() noexcept {
  if (g_thread_cache) {
    return;
  }

  g_thread_cache = create_cache(sizeof(Thread), alignof(Thread));
  g_fpu_cache = create_cache(hw::fpu::area_size(), hw::fpu::AREA_ALIGNMENT);

  if (hw::fpu::amx_available()) {
    g_amx_fpu_cache = create_cache(hw::fpu::area_size(true), hw::fpu::AREA_ALIGNMENT);
  }

  memory::register_shrinker(g_stack_shrinker);
}

StackCacheStats Thread::stack_cache_stats() noexcept { return g_stacks.stats(); }

void Thread::release_fpu_storage() noexcept {
  memory::heap::kfree(m_arch.fpu_storage);
  m_arch.fpu_storage = nullptr;
  m_arch.fpu_buffer_size = 0;
}

void Thread::release_user_stack() noexcept {
  if (m_user_stack_top.is_null() || m_process == nullptr) {
    return;
  }

  // By its top, which stays where it is when the stack grows down.
  memory::vmm::AddressSpace* space = m_process->address_space();
  if (space != nullptr) {
    if (const auto res = space->free_containing(m_user_stack_top - 1); !res) [[unlikely]] {
      utils::logger::warn("sched: leaking the user stack of thread {}: {:#x}\n", m_tid,
                          std::to_underlying(res.error()));
    }
  }

  m_user_stack_base = {};
  m_user_stack_top = {};
}

Thread::~Thread() noexcept {
  release_fpu_storage();
  memory::heap::kfree(std::exchange(m_resume, nullptr));  // never got to run

  if (!m_stack_base.is_null() && !flags().get_stack_borrowed()) {
    if (m_stack_size == DEFAULT_KSTACK_SIZE && g_stacks.put(m_stack_base)) {
      return;
    }

    if (const auto res = memory::vmm::kernel_space()->free(m_stack_base); !res) [[unlikely]] {
      utils::logger::warn("sched: leaking the stack of thread {}: {:#x}\n", m_tid,
                          std::to_underlying(res.error()));
    }
  }
}

auto Thread::create_kernel(std::string_view name, RawEntryFn entry, void* arg,
                           const ThreadCreateOptions& opts) noexcept
    -> std::expected<Thread*, Error> {
  if (!entry || opts.stack_size < MIN_KSTACK_SIZE) {
    return std::unexpected(Error::InvalidArgument);
  }

  if (!utils::maths::is_aligned(opts.stack_size, STACK_ALIGNMENT)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto* thread = allocate_thread();
  if (!thread) {
    return std::unexpected(Error::OutOfMemory);
  }

  // The VMM would round the size up all the same. This way, the top of the stack is at the top.
  const std::size_t stack_size = utils::maths::align_up(opts.stack_size, memory::PAGE_SIZE);

  memory::VirtualAddress stack_base =
      stack_size == DEFAULT_KSTACK_SIZE ? g_stacks.take() : memory::VirtualAddress{};
  if (stack_base.is_null()) {
    using memory::vmm::AccessFlags;
    const auto stack_res = memory::vmm::kernel_space()->alloc(
        stack_size,
        AccessFlags::Read | AccessFlags::Write | AccessFlags::Stack | AccessFlags::Populate);
    if (!stack_res) {
      free_thread(thread);
      return std::unexpected(stack_res.error());
    }

    stack_base = *stack_res;
  }

  thread->m_stack_base = stack_base;
  thread->m_stack_top = stack_base + static_cast<memory::VirtualAddress::OffsetType>(stack_size);
  thread->m_stack_size = stack_size;

  thread->m_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
  thread->m_entry = entry;
  thread->m_arg = arg;
  thread->set_cpu_id(opts.preferred_cpu != NO_PREFERENCE ? opts.preferred_cpu : hw::percpu::id());
  thread->m_preferred_numa_node = opts.preferred_numa_node;
  thread->m_base_priority = Priority::of(opts.priority);
  thread->m_effective_priority = thread->m_base_priority;
  thread->set_weight(priority_to_weight(opts.priority));
  thread->set_slice_ns(opts.slice_ns);
  thread->m_arch.fs_base = opts.initial_fs_base;
  thread->m_stats.creation_time_ns = drivers::SystemClock::now().raw_ns();

  ThreadFlags initial_flags;
  initial_flags.set_state(ThreadState::Ready);
  initial_flags.set_priority_class(opts.priority);
  initial_flags.set_is_kthread();
  initial_flags.set_is_idle(opts.priority == PriorityClass::Idle);
  initial_flags.set_is_pinned(opts.pinned);
  initial_flags.set_nice_bias(20);

  thread->m_flags.store(initial_flags.raw(), std::memory_order_release);
  thread->set_name(name);

  if (opts.allocate_amx || opts.allocate_fpu) {
    const auto fpu_res =
        opts.allocate_amx ? thread->request_amx_context() : thread->enable_fpu_context();
    if (!fpu_res) {
      free_thread(thread);
      return std::unexpected(fpu_res.error());
    }
  }

  thread->prepare_initial_stack();
  return thread;
}

auto Thread::create_user_half(const std::string_view name, proc::Process* process,
                              const UserThreadCreateOptions& opts,
                              const std::size_t extra_charge) noexcept
    -> std::expected<Thread*, Error> {
  // What the kernel holds for the thread counts against its process's tree, and is refused before
  // anything is allocated. Its user stack is memory of the address space, which counts that.
  const std::size_t kernel_stack =
      utils::maths::align_up(opts.kernel_stack_size, memory::PAGE_SIZE);
  auto charge = memory::QuotaCharge::take(
      process->quota(),
      sizeof(Thread) + kernel_stack + hw::fpu::area_size(opts.allocate_amx) + extra_charge);
  if (!charge) [[unlikely]] {
    return std::unexpected(charge.error());
  }

  const ThreadCreateOptions kernel_opts{
      .stack_size = opts.kernel_stack_size,
      .priority = opts.priority,
      .preferred_cpu = opts.preferred_cpu,
      .preferred_numa_node = opts.preferred_numa_node,
      .slice_ns = opts.slice_ns,
      .pinned = opts.pinned,
      .allocate_fpu = true,
      .allocate_amx = opts.allocate_amx,
      .initial_fs_base = opts.initial_fs_base,
  };

  const auto created = create_kernel(name, &user_thread_start, nullptr, kernel_opts);
  if (!created) {
    return created;
  }

  Thread* thread = *created;
  thread->m_charge = std::move(*charge);
  thread->m_arg = thread;
  thread->m_process = process;
  return thread;
}

auto Thread::create_user(const std::string_view name, proc::Process* process,
                         const memory::VirtualAddress entry, const std::uint64_t arg,
                         const UserThreadCreateOptions& opts) noexcept
    -> std::expected<Thread*, Error> {
  if (process == nullptr || process->address_space() == nullptr ||
      !hw::user::is_user_address(entry) || opts.user_stack_size == 0) {
    return std::unexpected(Error::InvalidArgument);
  }

  const auto created = create_user_half(name, process, opts, 0);
  if (!created) {
    return created;
  }

  Thread* thread = *created;
  const std::size_t stack_size = utils::maths::align_up(opts.user_stack_size, memory::PAGE_SIZE);

  using memory::vmm::AccessFlags;
  const auto stack = process->address_space()->alloc(
      stack_size, AccessFlags::Read | AccessFlags::Write | AccessFlags::User | AccessFlags::Stack);
  if (!stack) {
    (void)destroy(thread);
    return std::unexpected(stack.error());
  }

  thread->m_user_entry = entry;
  thread->m_user_arg = arg;
  thread->m_user_stack_base = *stack;
  thread->m_user_stack_top = *stack + static_cast<memory::VirtualAddress::OffsetType>(stack_size);
  thread->m_arch.user_gs_base = opts.initial_gs_base;
  thread->update_flags([](ThreadFlags f) { return f.without_is_kthread(); });

  return thread;
}

auto Thread::create_user_copy(const std::string_view name, proc::Process* process,
                              const hw::user::ResumeFrame& at) noexcept
    -> std::expected<Thread*, Error> {
  if (process == nullptr || process->address_space() == nullptr ||
      !hw::user::is_user_address(memory::VirtualAddress{at.rip}) ||
      at.rsp >= hw::user::lower_half_end()) {
    return std::unexpected(Error::InvalidArgument);
  }

  Thread* const original = current();
  if (original->is_kthread() || original->m_arch.fpu_storage == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  auto* resume = static_cast<hw::user::ResumeFrame*>(
      memory::heap::kmalloc(sizeof(hw::user::ResumeFrame), alignof(hw::user::ResumeFrame)));
  if (resume == nullptr) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  *resume = at;

  const UserThreadCreateOptions opts{
      .kernel_stack_size = original->m_stack_size,
      .priority = original->base_priority().priority_class,
      .allocate_amx = original->uses_amx(),
  };

  const auto created = create_user_half(name, process, opts, sizeof(hw::user::ResumeFrame));
  if (!created) {
    memory::heap::kfree(resume);
    return created;
  }

  Thread* thread = *created;
  thread->m_resume = resume;

  // What the CPU holds for the caller, as it is right now. A switch in between would read the
  // bases back from another thread, so there is none. The caller's FPU registers are its own here,
  // outside a kernel FPU section, and XSAVES leaves them as they are.
  {
    hw::percpu::preempt_disable();
    const ArchitecturalState& from = original->m_arch;
    ArchitecturalState& to = thread->m_arch;
    KASSERT(to.fpu_buffer_size == from.fpu_buffer_size && to.xsave_mask == from.xsave_mask);

    hw::fpu::saves(from.fpu_storage, from.xsave_mask);
    klib::memcpy(to.fpu_storage, from.fpu_storage, from.fpu_buffer_size);

    const bool bases_may_differ = hw::user::can_change_bases();
    to.fs_base = bases_may_differ ? hw::read::fs_base() : from.fs_base;
    to.user_gs_base = bases_may_differ ? hw::user::read_gs_base() : from.user_gs_base;
    to.pkru = hw::user::read_pkru();
    hw::percpu::preempt_enable();
  }

  // The address space copied the caller's stack to where it was. That copy is this thread's now,
  // and goes with it.
  thread->m_user_stack_base = original->m_user_stack_base;
  thread->m_user_stack_top = original->m_user_stack_top;
  thread->update_flags([](ThreadFlags f) { return f.without_is_kthread(); });

  return thread;
}

void Thread::user_thread_start(void* self) noexcept {
  auto* thread = static_cast<Thread*>(self);

  // A copy of another thread carries on where that one was. The frame goes off the heap first,
  // with interrupts still on, and onto this stack, which is about to be given up for ring 3.
  hw::user::ResumeFrame* const resume = std::exchange(thread->m_resume, nullptr);
  hw::user::ResumeFrame frame{};
  if (resume != nullptr) {
    frame = *resume;
    memory::heap::kfree(resume);
    thread->m_charge.shrink(sizeof(hw::user::ResumeFrame));
  }

  // Exiting or being preempted cannot wait until the first interrupt from ring 3.
  hw::irq::disable();
  Scheduler::return_to_user();

  if (resume != nullptr) {
    hw::user::resume(frame);
  }

  hw::user::enter(thread->m_user_entry,
                  thread->m_user_stack_top -
                      static_cast<memory::VirtualAddress::OffsetType>(sizeof(std::uint64_t)),
                  thread->m_user_arg);
}

auto Thread::adopt_boot_thread(const std::uint32_t cpu_id, const memory::VirtualAddress stack_base,
                               const std::size_t stack_size, const std::string_view name) noexcept
    -> std::expected<Thread*, Error> {
  KASSERT(cpu_id == hw::percpu::id());

  if (stack_base.is_null() || stack_size < MIN_KSTACK_SIZE) {
    return std::unexpected(Error::InvalidArgument);
  }

  if (current() != nullptr) {
    return std::unexpected(Error::InvalidState);
  }

  auto* thread = allocate_thread();
  if (!thread) {
    return std::unexpected(Error::OutOfMemory);
  }

  thread->m_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
  thread->m_stack_base = stack_base;
  thread->m_stack_top = stack_base + static_cast<memory::VirtualAddress::OffsetType>(stack_size);
  thread->m_stack_size = stack_size;
  thread->set_cpu_id(cpu_id);
  thread->m_base_priority = Priority::of(PriorityClass::Idle);
  thread->m_effective_priority = thread->m_base_priority;
  thread->set_weight(priority_to_weight(PriorityClass::Idle));
  thread->m_preferred_numa_node = hw::percpu::numa_node_id();
  thread->m_arch.fs_base = hw::read::fs_base();

  thread->m_expected_canary = generate_stack_canary();
  *thread->m_stack_base.as<volatile std::uint64_t>() = thread->m_expected_canary;

  const auto now = drivers::SystemClock::now().raw_ns();
  thread->m_stats.creation_time_ns = now;
  thread->m_stats.last_switch_in_ns = now;
  thread->m_stats.switch_count = 1;

  ThreadFlags boot_flags;
  boot_flags.set_state(ThreadState::Running);
  boot_flags.set_priority_class(PriorityClass::Idle);
  boot_flags.set_is_kthread();
  boot_flags.set_is_idle(true);
  boot_flags.set_is_pinned(true);
  boot_flags.set_stack_borrowed(true);
  boot_flags.set_nice_bias(20);

  thread->m_flags.store(boot_flags.raw(), std::memory_order_release);
  thread->set_name(name);

  hw::percpu::set_current_thread(thread);
  return thread;
}

auto Thread::destroy(Thread* thread) noexcept -> std::expected<void, Error> {
  if (!thread) {
    return std::unexpected(Error::InvalidArgument);
  }

  const ThreadState state = thread->state();
  const bool never_ran = state == ThreadState::Created ||
                         (state == ThreadState::Ready && thread->m_stats.switch_count == 0);
  const ThreadFlags flags = thread->flags();
  const bool scheduler_has_it = flags.get_on_runqueue() || flags.get_scheduled();
  if (thread == current() || (state != ThreadState::Dead && (!never_ran || scheduler_has_it))) {
    return std::unexpected(Error::InvalidState);
  }

  // Someone who found it just before it finished may not have got round to letting go yet.
  while (thread->m_retained.load(std::memory_order_acquire) != 0) {
    hw::cpu_relax();
  }

  // Before the hook: a process may let go of its address space there, when its last thread goes.
  thread->release_user_stack();

  if (thread->m_on_destroy != nullptr) {
    thread->m_on_destroy(thread, thread->m_destroy_cookie);
  }

  free_thread(thread);
  return {};
}

auto Thread::enable_fpu_context() noexcept -> std::expected<void, Error> {
  if (m_arch.fpu_storage) {
    return {};
  }

  const auto area = g_fpu_cache->alloc();
  if (!area) {
    return std::unexpected(area.error());
  }

  const std::size_t size = hw::fpu::area_size();
  const std::uint64_t mask = hw::fpu::state_mask();
  klib::memset(*area, 0, size);
  hw::fpu::init_area(*area, mask);

  m_arch.xsave_mask = mask;
  m_arch.fpu_buffer_size = size;
  m_arch.fpu_storage = *area;

  update_flags([](ThreadFlags f) { return f.with_uses_fpu(); });
  return {};
}

auto Thread::request_amx_context() noexcept -> std::expected<void, Error> {
  if (!hw::fpu::amx_available()) {
    return std::unexpected(Error::NotImplemented);
  }

  if (uses_amx()) {
    return {};
  }

  // A user thread pays for the room tile data takes, on top of the area it had, if any.
  const std::size_t added =
      hw::fpu::area_size(true) - (m_arch.fpu_storage != nullptr ? m_arch.fpu_buffer_size : 0);
  if (const auto charged = m_charge.grow(added); !charged) [[unlikely]] {
    return charged;
  }

  const auto area = g_amx_fpu_cache->alloc();
  if (!area) {
    m_charge.shrink(added);
    return std::unexpected(area.error());
  }

  const std::size_t size = hw::fpu::area_size(true);
  const std::uint64_t mask = hw::fpu::state_mask(true);
  klib::memset(*area, 0, size);

  void* old_area;
  {
    // A switch to this thread, or away from it, must see all of this or none of it.
    utils::IrqDisableGuard irq_guard{};

    // Tile data is numbered above everything else, so what is already saved keeps its place in
    // the area, and tile data, not being in XSTATE_BV yet, starts out in its initial state.
    old_area = m_arch.fpu_storage;
    if (old_area) {
      klib::memcpy(*area, old_area, m_arch.fpu_buffer_size);
      hw::fpu::set_area_mask(*area, mask);
    } else {
      hw::fpu::init_area(*area, mask);
    }

    m_arch.xsave_mask = mask;
    m_arch.fpu_buffer_size = size;
    m_arch.fpu_storage = *area;
    m_arch.xfd = hw::fpu::Ia32Xfd{};

    update_flags([](ThreadFlags f) { return f.with_uses_fpu().with_uses_amx(); });

    if (current() == this) {
      switch_xfd(m_arch.xfd);
    }
  }

  memory::heap::kfree(old_area);
  return {};
}

KernelFpuSection Thread::begin_kernel_fpu() noexcept {
  KASSERT(current() == this);
  KASSERT(!hw::percpu::can_sleep());
  KASSERT(!hw::percpu::in_interrupt());

  const ThreadFlags before = flags();
  if (before.get_in_kernel_fpu()) {
    return KernelFpuSection::Nested;
  }

  hw::percpu::preempt_disable();

  const bool save = before.get_uses_fpu() && !before.get_is_kthread();
  if (save) {
    hw::fpu::saves(m_arch.fpu_storage, m_arch.xsave_mask);
  }

  update_flags([](ThreadFlags f) { return f.with_in_kernel_fpu(); });
  return save ? KernelFpuSection::Saved : KernelFpuSection::Direct;
}

void Thread::end_kernel_fpu(const KernelFpuSection section) noexcept {
  if (section == KernelFpuSection::Nested) {
    return;
  }

  // What a kernel thread leaves behind only needs saving if it ever leaves the CPU, so that is
  // when it happens.
  const bool dirty = section == KernelFpuSection::Direct && uses_fpu();
  update_flags([dirty](ThreadFlags f) { return f.without_in_kernel_fpu().with_fpu_dirty(dirty); });

  if (section == KernelFpuSection::Saved) {
    hw::fpu::restores(m_arch.fpu_storage, m_arch.xsave_mask);
  }

  hw::percpu::preempt_enable();
}

void Thread::prepare_initial_stack() noexcept {
  m_expected_canary = generate_stack_canary();
  *m_stack_base.as<volatile std::uint64_t>() = m_expected_canary;

  // A zero return address, so that backtraces stop here.
  auto sp = m_stack_top.align_down(STACK_ALIGNMENT);
  sp -= sizeof(std::uint64_t);
  *sp.as<std::uint64_t>() = 0;

  sp -= sizeof(SwitchContext);
  *sp.as<SwitchContext>() = SwitchContext{
      .r12 = reinterpret_cast<std::uint64_t>(this),
      .rip = reinterpret_cast<std::uint64_t>(&thread_entry_trampoline),
  };

  m_rsp = sp;
}

void Thread::set_name(std::string_view name) noexcept {
  m_name.fill('\0');
  const std::size_t copy_len = (name.size() < (MAX_NAME_LEN - 1)) ? name.size() : MAX_NAME_LEN - 1;
  if (copy_len > 0) {
    klib::memcpy(m_name.data(), name.data(), copy_len);
  }
}

[[noreturn]] void Thread::entry_launchpad(Thread* self) noexcept {
  // switch_to() disabled both of these for a return that a new thread does not make, and so did
  // the scheduler if it is the one that got here, along with leaving the run queue locked.
  Scheduler::finish_switch();
  hw::irq::enable();
  hw::percpu::preempt_enable();

  self->m_entry(self->m_arg);

  self->set_state(ThreadState::Dead);

  if (self->m_on_exit) {
    self->m_on_exit(self, self->m_exit_cookie);
  }

  Scheduler::exit_current();
}

Thread* current() noexcept { return hw::percpu::current_thread(); }

void switch_to(Thread* prev, Thread* next, const SwitchReason reason) noexcept {
  KASSERT(prev && next);
  utils::rcu::note_quiescent_state();
  if (prev == next) {
    return;
  }

  utils::IrqDisableGuard irq_guard{};

  KASSERT(prev == current());
  KASSERT(!prev->in_kernel_fpu());

  if (!prev->verify_stack_canary()) [[unlikely]] {
    utils::logger::fatal("sched: thread {} ran off the end of its stack\n", prev->tid());
  }

  if (!next->verify_stack_canary() || !next->is_stack_pointer_valid(next->saved_rsp()))
      [[unlikely]] {
    utils::logger::fatal("sched: thread {} has a corrupted stack\n", next->tid());
  }

  ThreadStats& prev_stats = prev->stats();
  ThreadStats& next_stats = next->stats();

  const std::uint64_t now = drivers::SystemClock::now().raw_ns();
  if (prev_stats.last_switch_in_ns != 0 && now >= prev_stats.last_switch_in_ns) {
    prev_stats.total_runtime_ns += now - prev_stats.last_switch_in_ns;
  }

  next_stats.last_switch_in_ns = now;
  next_stats.switch_count += 1;
  if (reason == SwitchReason::Voluntary) {
    prev_stats.voluntary_switches += 1;
  } else {
    prev_stats.involuntary_switches += 1;
  }

  ArchitecturalState& prev_arch = prev->arch_state();
  const ArchitecturalState& next_arch = next->arch_state();
  const ThreadFlags prev_flags = prev->flags();

  if (!prev_flags.get_is_kthread()) {
    save_user_state(prev_arch);
  }

  // In this order: XSAVE takes a component disabled in IA32_XFD to be in its initial state, and
  // XRSTOR faults on one. Each side has to do its part under its own XFD.
  if (has_unsaved_fpu_state(prev_flags)) {
    hw::fpu::saves(prev_arch.fpu_storage, prev_arch.xsave_mask);
  }

  if (prev_arch.xfd.raw() != next_arch.xfd.raw()) {
    switch_xfd(next_arch.xfd);
  }

  if (next->uses_fpu()) {
    hw::fpu::restores(next_arch.fpu_storage, next_arch.xsave_mask);
  }

  if (next->is_user()) {
    load_user_context(*next);
  }

  if (prev_arch.fs_base != next_arch.fs_base) {
    hw::write::fs_base(next_arch.fs_base);
  }

  if (prev_arch.pkru != next_arch.pkru) {
    switch_pkru(next_arch.pkru);
  }

  prev->update_flags([](ThreadFlags f) {
    return f.with_state(f.get_state() == ThreadState::Running ? ThreadState::Ready : f.get_state())
        .without_fpu_dirty();
  });

  next->update_flags([](ThreadFlags f) { return f.with_state(ThreadState::Running); });

  next->set_cpu_id(hw::percpu::id());
  hw::percpu::set_current_thread(next);

  switch_context(reinterpret_cast<std::uint64_t*>(prev->saved_rsp_ptr()),
                 next->saved_rsp().value());
}
}  // namespace kernel::sched
