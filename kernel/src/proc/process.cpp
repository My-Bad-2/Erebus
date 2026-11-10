#include "proc/process.hpp"

#include <string.h>

#include <algorithm>
#include <memory>

#include "drivers/clock.hpp"
#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "memory/heap.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "sched/scheduler.hpp"
#include "utils/logger.hpp"

namespace kernel::proc {
namespace {
memory::heap::KmemCache* g_process_cache = nullptr;
Process* g_kernel_process = nullptr;
std::atomic g_next_pid{KERNEL_PID + 1};
constinit utils::QSpinlock g_family_lock{};

[[nodiscard]] memory::vmm::AddressSpace* current_user_space() noexcept {
  const Process* process = sched::current()->process();
  return process != nullptr ? process->address_space() : nullptr;
}

void on_user_fault(const hw::interrupts::Event& event) noexcept {
  Process* process = sched::current()->process();
  if (process == nullptr) [[unlikely]] {
    utils::logger::fatal("proc: exception {} from ring 3 on thread {}, which has no process\n",
                         event.vector, sched::current()->tid());
  }

  utils::logger::warn("proc: {} (pid {}) ends on exception {} at {:#x} (error {:#x}, data {:#x})\n",
                      process->name(), process->pid(), event.vector, event.rip, event.error_code,
                      event.payload);

  // The thread goes on its way back to ring 3, where exit() makes sure it never arrives.
  process->exit(EXIT_FAULT);
}

void free_address_space(memory::vmm::AddressSpace* space) noexcept {
  if (space != nullptr) {
    std::destroy_at(space);
    memory::heap::kfree(space);
  }
}
}  // namespace

Process* kernel_process() noexcept { return g_kernel_process; }

Process* current_process() noexcept {
  Process* process = sched::current()->process();
  return process != nullptr ? process : g_kernel_process;
}

Process* Process::allocate(const std::string_view name) noexcept {
  const auto memory = g_process_cache->alloc();
  if (!memory) {
    return nullptr;
  }

  auto* process = new (*memory) Process();
  const std::size_t length = std::min(name.size(), MAX_NAME_LEN - 1);
  klib::memcpy(process->m_name.data(), name.data(), length);
  return process;
}

void Process::free(Process* process) noexcept {
  // The tree is done with it. Whoever else holds a reference finds it Dead from here on.
  process->m_state.store(ProcessState::Dead, std::memory_order_release);
  process->release();
}

void Process::release() noexcept {
  if (m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    this->~Process();
    g_process_cache->free(this);
  }
}

memory::vmm::AddressSpace* Process::acquire_address_space() noexcept {
  utils::IrqSaveGuard guard{m_lock};
  if (m_address_space != nullptr) {
    m_space_users.fetch_add(1, std::memory_order_acquire);
  }

  return m_address_space;
}

void Process::destroy_address_space() noexcept {
  memory::vmm::AddressSpace* space;
  {
    utils::IrqSaveGuard guard{m_lock};
    space = std::exchange(m_address_space, nullptr);
  }

  // Nobody borrows it from here on; those who did have it for a moment more.
  while (m_space_users.load(std::memory_order_acquire) != 0) {
    sched::Scheduler::yield();
  }

  free_address_space(space);
}

auto Process::create(const std::string_view name, Process* parent) noexcept
    -> std::expected<Process*, Error> {
  Process* const owner = parent != nullptr ? parent : current_process();

  Process* process = allocate(name);
  void* space_memory =
      memory::heap::kmalloc(sizeof(memory::vmm::AddressSpace), alignof(memory::vmm::AddressSpace));
  if (process == nullptr || space_memory == nullptr) {
    memory::heap::kfree(space_memory);
    if (process != nullptr) {
      free(process);
    }

    return std::unexpected(Error::OutOfMemory);
  }

  process->m_address_space =
      std::construct_at(static_cast<memory::vmm::AddressSpace*>(space_memory),
                        memory::VirtualAddress{USER_SPACE_BASE});

  bool adopted = false;
  {
    utils::IrqSaveGuard guard{g_family_lock};

    // A parent that is a zombie already gave its children away, and would never see this one.
    if (owner->state() < ProcessState::Zombie) {
      process->m_pid = g_next_pid.fetch_add(1, std::memory_order_relaxed);
      process->m_parent = owner;
      owner->m_children.push_front(*process);
      adopted = true;
    }
  }

  if (!adopted) {
    free_address_space(std::exchange(process->m_address_space, nullptr));
    free(process);
    return std::unexpected(Error::InvalidState);
  }

  return process;
}

auto Process::spawn_thread(const std::string_view name, const memory::VirtualAddress entry,
                           const std::uint64_t arg,
                           const sched::UserThreadCreateOptions& opts) noexcept
    -> std::expected<sched::Thread*, Error> {
  return spawn_thread(name, entry, arg, opts, nullptr, nullptr);
}

auto Process::spawn_thread(const std::string_view name, const memory::VirtualAddress entry,
                           const std::uint64_t arg, const sched::UserThreadCreateOptions& opts,
                           const ThreadSetup setup, void* const setup_context) noexcept
    -> std::expected<sched::Thread*, Error> {
  // The thread counts from here on, so the address space stays while it is being made, whatever
  // exit() does in the meantime.
  {
    utils::IrqSaveGuard guard{m_lock};
    if (m_address_space == nullptr ||
        m_state.load(std::memory_order_relaxed) != ProcessState::Running) {
      return std::unexpected(Error::InvalidState);
    }

    m_live_threads.fetch_add(1, std::memory_order_relaxed);
  }

  const auto created = sched::Thread::create_user(name, this, entry, arg, opts);
  if (!created) {
    bool last;
    {
      utils::IrqSaveGuard guard{m_lock};
      last = m_live_threads.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
             m_state.load(std::memory_order_relaxed) == ProcessState::Exiting;
    }

    if (last) {
      finish();
    }

    return created;
  }

  sched::Thread* thread = *created;
  thread->set_destroy_hook(&on_thread_destroyed, this);

  // Destroying it takes care of the count, as below.
  if (setup != nullptr) {
    if (const auto ready = setup(*thread, setup_context); !ready) {
      (void)sched::Thread::destroy(thread);
      return std::unexpected(ready.error());
    }
  }

  bool refused;
  {
    utils::IrqSaveGuard guard{m_lock};
    refused = m_state.load(std::memory_order_relaxed) != ProcessState::Running;
    if (!refused) {
      m_threads.push_front(*thread);
    }
  }

  // Either way, destroying it takes care of the count.
  if (refused || !sched::Scheduler::enqueue(thread)) {
    (void)sched::Thread::destroy(thread);
    return std::unexpected(Error::InvalidState);
  }

  return thread;
}

auto Process::fork(const std::string_view name, const memory::VirtualAddress entry,
                   const std::uint64_t arg, const sched::UserThreadCreateOptions& opts) noexcept
    -> std::expected<Process*, Error> {
  if (m_address_space == nullptr) {
    return std::unexpected(Error::InvalidState);
  }

  const auto child = create(name, this);
  if (!child) {
    return child;
  }

  if (const auto cloned = m_address_space->clone_into(*(*child)->m_address_space); !cloned) {
    (*child)->discard();
    return std::unexpected(cloned.error());
  }

  if (const auto thread = (*child)->spawn_thread(name, entry, arg, opts); !thread) {
    (*child)->discard();
    return std::unexpected(thread.error());
  }

  return *child;
}

void Process::exit(const int status) noexcept {
  if (this == g_kernel_process) [[unlikely]] {
    return;
  }

  bool finish_now;
  {
    utils::IrqSaveGuard guard{m_lock};
    const ProcessState state = m_state.load(std::memory_order_relaxed);
    if (state != ProcessState::Running && state != ProcessState::Stopped) {
      return;
    }

    m_exit_status = status;
    m_state.store(ProcessState::Exiting, std::memory_order_release);

    for (sched::Thread& thread : m_threads) {
      thread.request_exit();

      // A thread in ring 3 does not come back by itself; an interrupt brings it in, and the way
      // out of that ends it. One on this CPU is the one calling, on its way out already.
      const std::uint32_t cpu = thread.cpu_id();
      if (thread.state() == sched::ThreadState::Running && cpu != hw::percpu::id()) {
        hw::ipi::send_reschedule(cpu);
      }
    }

    finish_now = m_live_threads.load(std::memory_order_relaxed) == 0;
  }

  if (!finish_now) {
    return;
  }

  // Nothing ever ran, so the rest is up to this call. Letting go of an address space waits for
  // other CPUs, which an interrupt handler must not.
  if (hw::percpu::can_sleep() && hw::irq::is_enabled()) {
    finish();
  } else {
    (void)sched::workqueue::queue(m_finish);
  }
}

void Process::on_thread_destroyed(sched::Thread* thread, void* process) noexcept {
  auto* self = static_cast<Process*>(process);
  bool last;
  {
    utils::IrqSaveGuard guard{self->m_lock};
    (void)self->m_threads.remove(*thread);
    last = self->m_live_threads.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
           self->m_state.load(std::memory_order_relaxed) == ProcessState::Exiting;
  }

  if (last) {
    self->finish();
  }
}

void Process::finish_later(void* process) noexcept { static_cast<Process*>(process)->finish(); }

void Process::finish() noexcept {
  // The last thread is destroyed, so nothing can be in the address space any more, and nothing
  // can come along and start a new one.
  destroy_address_space();

  utils::SList<Process, ProcessChildTag> orphans_done;
  bool free_self = false;
  {
    utils::IrqSaveGuard guard{g_family_lock};

    // Process 0 takes the children, and never waits for any of them: they go when they are done,
    // and those that are done already go right now.
    while (!m_children.empty()) {
      Process* child = m_children.pop_front();
      child->m_parent = g_kernel_process;
      child->m_detached = true;

      if (child->state() == ProcessState::Zombie) {
        orphans_done.push_front(*child);
      } else {
        g_kernel_process->m_children.push_front(*child);
      }
    }

    m_state.store(ProcessState::Zombie, std::memory_order_release);

    if (m_detached) {
      (void)m_parent->m_children.remove(*this);
      free_self = true;
    } else {
      (void)m_parent->m_child_exit_wq.wake_all();
    }
  }

  while (!orphans_done.empty()) {
    free(orphans_done.pop_front());
  }

  if (free_self) {
    free(this);
  }
}

void Process::discard() noexcept {
  {
    utils::IrqSaveGuard guard{g_family_lock};
    (void)m_parent->m_children.remove(*this);
  }

  destroy_address_space();
  free(this);
}

bool Process::has_child_locked(const ProcessId target) const noexcept {
  for (const Process& child : m_children) {
    if (target == ANY_CHILD || child.m_pid == target) {
      return true;
    }
  }

  return false;
}

bool Process::reap_locked(const ProcessId target, int* out_status, ProcessId& found,
                          Process*& zombie) noexcept {
  for (Process& child : m_children) {
    if ((target == ANY_CHILD || child.m_pid == target) && child.state() == ProcessState::Zombie) {
      (void)m_children.remove(child);
      found = child.m_pid;
      zombie = &child;
      if (out_status != nullptr) {
        *out_status = child.m_exit_status;
      }

      return true;
    }
  }

  return false;
}

auto Process::wait_child(const ProcessId target, int* out_status) noexcept
    -> std::expected<ProcessId, Error> {
  return wait_child_for(target, klib::Duration::nanoseconds(INT64_MAX), out_status);
}

auto Process::wait_child_for(const ProcessId target, const klib::Duration timeout,
                             int* out_status) noexcept -> std::expected<ProcessId, Error> {
  const bool forever = timeout == klib::Duration::nanoseconds(INT64_MAX);
  const klib::TimePoint deadline =
      forever ? klib::TimePoint{} : drivers::SystemClock::now() + timeout;

  const auto settled = [&] {
    utils::IrqSaveGuard guard{g_family_lock};
    for (const Process& child : m_children) {
      if ((target == ANY_CHILD || child.m_pid == target) && child.state() == ProcessState::Zombie) {
        return true;
      }
    }

    return !has_child_locked(target);
  };

  while (true) {
    ProcessId found = 0;
    Process* zombie = nullptr;
    {
      utils::IrqSaveGuard guard{g_family_lock};
      if (!reap_locked(target, out_status, found, zombie) && !has_child_locked(target)) {
        return std::unexpected(Error::NotFound);
      }
    }

    if (zombie != nullptr) {
      free(zombie);
      return found;
    }

    if (forever) {
      m_child_exit_wq.wait(settled);
    } else if (!m_child_exit_wq.wait_until(deadline, settled)) {
      return std::unexpected(Error::Timeout);
    }
  }
}

void initialize() noexcept {
  // A process can go away while something still has a pointer to it, which is a bug that would
  // otherwise read whatever the freed memory held and carry on.
  const auto cache = memory::heap::KmemCache::create(sizeof(Process), alignof(Process),
                                                     memory::heap::CacheFlags::Poison);
  if (!cache) {
    utils::logger::fatal("proc: cannot create the process cache: {:#x}\n",
                         std::to_underlying(cache.error()));
  }

  g_process_cache = *cache;
  g_kernel_process = Process::allocate("kernel");
  if (g_kernel_process == nullptr) {
    utils::logger::fatal("proc: no memory for process 0\n");
  }

  memory::vmm::set_user_space_lookup(&current_user_space);
  hw::interrupts::InterruptManager::set_user_fault_handler(&on_user_fault);
}
}  // namespace kernel::proc
