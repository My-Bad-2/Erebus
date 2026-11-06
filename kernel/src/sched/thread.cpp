#include "sched/thread.hpp"

#include <memory>
#include <span>

#include "crypto/random.hpp"
#include "drivers/clock.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "hal/smp/percpu.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/maths.hpp"

namespace kernel::sched {
namespace {
std::atomic<ThreadId> g_next_tid{1};

memory::heap::KmemCache* g_thread_cache{nullptr};
memory::heap::KmemCache* g_fpu_cache{nullptr};
memory::heap::KmemCache* g_amx_fpu_cache{nullptr};  // null without AMX

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
void update_ring0_stack_legacy(const memory::VirtualAddress stack_top) noexcept {
  hw::percpu::self()->gdt_table.set_kernel_stack(stack_top);
}

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
}

void Thread::release_fpu_storage() noexcept {
  memory::heap::kfree(m_arch.fpu_storage);
  m_arch.fpu_storage = nullptr;
  m_arch.fpu_buffer_size = 0;
}

Thread::~Thread() noexcept {
  release_fpu_storage();

  if (!m_stack_base.is_null() && !flags().get_stack_borrowed()) {
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

  using memory::vmm::AccessFlags;
  const auto stack_res = memory::vmm::kernel_space()->alloc(
      stack_size,
      AccessFlags::Read | AccessFlags::Write | AccessFlags::Stack | AccessFlags::Populate);
  if (!stack_res) {
    free_thread(thread);
    return std::unexpected(stack_res.error());
  }

  thread->m_stack_base = *stack_res;
  thread->m_stack_top = *stack_res + static_cast<memory::VirtualAddress::OffsetType>(stack_size);
  thread->m_stack_size = stack_size;

  thread->m_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
  thread->m_entry = entry;
  thread->m_arg = arg;
  thread->m_cpu_id = opts.preferred_cpu;
  thread->m_preferred_numa_node = opts.preferred_numa_node;
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
  thread->m_cpu_id = cpu_id;
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
  if (thread == current() || (state != ThreadState::Dead && !never_ran)) {
    return std::unexpected(Error::InvalidState);
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

  const auto area = g_amx_fpu_cache->alloc();
  if (!area) {
    return std::unexpected(area.error());
  }

  const std::size_t size = hw::fpu::area_size(true);
  const std::uint64_t mask = hw::fpu::state_mask(true);
  klib::memset(*area, 0, size);

  void* old_area = nullptr;
  {
    // A switch to this thread, or away from it, must see all of this or none of it.
    utils::IrqDisableGuard irq_guard{};

    // AMX is numbered above everything else, so what is already saved keeps its place in the area
    // and the new components, not being in XSTATE_BV yet, start out in their initial state.
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
  // switch_to() disabled both of these for a return that a new thread does not make.
  hw::percpu::preempt_enable();
  hw::irq::enable();

  self->m_entry(self->m_arg);

  self->set_state(ThreadState::Dead);

  if (self->m_on_exit) {
    self->m_on_exit(self, self->m_exit_cookie);
  }

  while (true) {
    asm volatile("hlt");
  }
}

Thread* current() noexcept { return hw::percpu::current_thread(); }

void switch_to(Thread* prev, Thread* next) noexcept {
  KASSERT(prev && next);
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
  prev_stats.voluntary_switches += 1;

  ArchitecturalState& prev_arch = prev->arch_state();
  const ArchitecturalState& next_arch = next->arch_state();

  // In this order: XSAVE takes a component disabled in IA32_XFD to be in its initial state, and
  // XRSTOR faults on one. Each side has to do its part under its own XFD.
  if (has_unsaved_fpu_state(prev->flags())) {
    hw::fpu::saves(prev_arch.fpu_storage, prev_arch.xsave_mask);
  }

  if (prev_arch.xfd.raw() != next_arch.xfd.raw()) {
    switch_xfd(next_arch.xfd);
  }

  if (next->uses_fpu()) {
    hw::fpu::restores(next_arch.fpu_storage, next_arch.xsave_mask);
  }

  update_ring0_stack(next->stack_top());

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
