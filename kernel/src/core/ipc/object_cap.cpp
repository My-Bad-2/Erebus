#include "core/ipc/object_cap.hpp"

#include <utility>

#include "sched/scheduler.hpp"

namespace kernel::core::ipc {
namespace {
using Guard = utils::IrqSaveGuard<utils::QSpinlock>;

struct SpawnSetup {
  CSpace* cspace;
  Endpoint* fault_endpoint;
  std::uint64_t fault_badge;
  CapRef<ThreadCap> result;
};

std::expected<void, Error> attach_spawned(sched::Thread& thread, void* context) noexcept {
  auto& setup = *static_cast<SpawnSetup*>(context);
  auto attached = ThreadCap::attach(thread, setup.cspace);
  if (!attached) [[unlikely]] {
    return std::unexpected(attached.error());
  }

  if (setup.fault_endpoint != nullptr) {
    (void)(*attached)->set_fault_endpoint(CapRights::Manage, setup.fault_endpoint,
                                          setup.fault_badge);
  }

  setup.result = std::move(*attached);
  return {};
}
}  // namespace

ThreadCap::ThreadCap(sched::Thread& thread, CSpace* const cspace) noexcept
    : m_thread(&thread), m_tid(thread.tid()), m_ipc(&thread, cspace, &m_buffer) {}

ThreadCap::~ThreadCap() noexcept {
  // The thread is gone: it held a reference until then.
  KASSERT(m_thread == nullptr);
  if (Endpoint* endpoint = std::exchange(m_fault_endpoint, nullptr)) {
    endpoint->release_ref();
  }
}

std::expected<CapRef<ThreadCap>, Error> ThreadCap::attach(sched::Thread& thread,
                                                          CSpace* const cspace) noexcept {
  auto cap = create(thread, cspace);
  if (!cap) [[unlikely]] {
    return cap;
  }

  ThreadCap& self = **cap;
  self.m_next_hook = thread.destroy_hook();
  self.m_next_cookie = thread.destroy_cookie();
  thread.set_destroy_hook(&ThreadCap::on_thread_destroyed, &self);
  thread.set_ipc_context(&self.m_ipc);

  self.add_ref();
  return cap;
}

void ThreadCap::on_thread_destroyed(sched::Thread* const thread, void* const self) noexcept {
  auto& cap = *static_cast<ThreadCap*>(self);
  {
    Guard guard{cap.m_lock};
    cap.m_thread = nullptr;
  }

  cap.m_ipc.abandon();

  if (cap.m_next_hook != nullptr) {
    cap.m_next_hook(thread, cap.m_next_cookie);
  }

  cap.release_ref();
}

std::expected<CapRef<ThreadCap>, Error> ThreadCap::create_kernel_thread(
    const std::string_view name, const sched::Thread::RawEntryFn entry, void* const arg,
    CSpace* const cspace, const sched::ThreadCreateOptions& opts) noexcept {
  const auto thread = sched::Thread::create_kernel(name, entry, arg, opts);
  if (!thread) [[unlikely]] {
    return std::unexpected(thread.error());
  }

  auto cap = attach(**thread, cspace);
  if (!cap) [[unlikely]] {
    (void)sched::Thread::destroy(*thread);
    return cap;
  }

  const auto started = sched::Scheduler::enqueue(*thread);
  if (!started) [[unlikely]] {
    (void)sched::Thread::destroy(*thread);
    return std::unexpected(started.error());
  }

  return cap;
}

bool ThreadCap::is_alive() const noexcept {
  Guard guard{m_lock};
  return m_thread != nullptr;
}

std::expected<void, Error> ThreadCap::request_exit(CapRights rights) const noexcept {
  if (!has_rights(rights, CapRights::Write)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  Guard guard{m_lock};
  if (m_thread == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  m_thread->request_exit();
  return {};
}

std::expected<sched::ThreadStats, Error> ThreadCap::read_stats(CapRights rights) const noexcept {
  if (!has_rights(rights, CapRights::Read)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  Guard guard{m_lock};
  if (m_thread == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return m_thread->stats();
}

std::expected<void, Error> ThreadCap::bind_notification(const CapRights rights,
                                                        Notification& notification) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (!is_alive()) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return notification.bind(m_ipc);
}

std::expected<void, Error> ThreadCap::unbind_notification(const CapRights rights) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const utils::rcu::ReadGuard rcu;
  if (Notification* bound = m_ipc.bound_notification()) {
    bound->unbind(m_ipc);
  }

  return {};
}

std::expected<void, Error> ThreadCap::set_fault_endpoint(const CapRights rights,
                                                         Endpoint* const endpoint,
                                                         const std::uint64_t badge) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (endpoint != nullptr) {
    endpoint->add_ref();
  }

  Endpoint* previous;
  {
    Guard guard{m_lock};
    previous = std::exchange(m_fault_endpoint, endpoint);
    m_fault_badge = badge;
  }

  if (previous != nullptr) {
    previous->release_ref();
  }

  return {};
}

std::expected<void, Error> ThreadCap::send_fault(const FaultMsgLabel label,
                                                 const std::uint64_t arg0, const std::uint64_t arg1,
                                                 const std::uint64_t rip) noexcept {
  KASSERT(m_ipc.thread() == sched::current());

  CapRef<Endpoint> endpoint;
  std::uint64_t badge;
  {
    Guard guard{m_lock};
    endpoint = CapRef<Endpoint>::share(m_fault_endpoint);
    badge = m_fault_badge;
  }

  if (!endpoint) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  const Message request{std::to_underlying(label), arg0, arg1, rip, m_tid};
  Message answer{};
  if (const auto called = endpoint->call(m_ipc, request, answer, badge, CapRights::Write); !called)
      [[unlikely]] {
    return called;
  }

  if (has_msg_flag(answer.tag.get_flags(), MsgFlags::KernelError) ||
      answer.tag.error() != Error::Success) [[unlikely]] {
    return std::unexpected(answer.tag.error() != Error::Success ? answer.tag.error()
                                                                : Error::PageFault);
  }

  return {};
}

class ProcessCap::SpaceLoan {
  proc::Process& m_process;
  memory::vmm::AddressSpace* m_space;

 public:
  explicit SpaceLoan(proc::Process& process) noexcept
      : m_process(process), m_space(process.acquire_address_space()) {}

  ~SpaceLoan() noexcept {
    if (m_space != nullptr) {
      m_process.release_address_space();
    }
  }

  SpaceLoan(const SpaceLoan&) = delete;
  SpaceLoan& operator=(const SpaceLoan&) = delete;

  [[nodiscard]] memory::vmm::AddressSpace* get() const noexcept { return m_space; }
};

ProcessCap::ProcessCap(proc::Process& process, CapRef<CSpace> cspace) noexcept
    : m_process(&process), m_pid(process.pid()), m_cspace(std::move(cspace)) {
  m_process->retain();
}

ProcessCap::~ProcessCap() noexcept {
  if (Endpoint* endpoint = std::exchange(m_fault_endpoint, nullptr)) {
    endpoint->release_ref();
  }

  m_process->release();
}

std::expected<CapRef<ProcessCap>, Error> ProcessCap::create(const std::string_view name,
                                                            proc::Process* const parent) noexcept {
  const auto process = proc::Process::create(name, parent);
  if (!process) [[unlikely]] {
    return std::unexpected(process.error());
  }

  auto cap = wrap(**process);
  if (!cap) [[unlikely]] {
    (*process)->exit(proc::EXIT_FAULT);
  }

  return cap;
}

std::expected<CapRef<ProcessCap>, Error> ProcessCap::wrap(proc::Process& process,
                                                          CSpace* const cspace) noexcept {
  CapRef<CSpace> space;
  if (cspace != nullptr) {
    space = CapRef<CSpace>::share(cspace);
  } else if (auto made = CSpace::create()) [[likely]] {
    space = std::move(*made);
  } else {
    return std::unexpected(made.error());
  }

  return TypedCapObject::create(process, std::move(space));
}

std::expected<CapRef<ThreadCap>, Error> ProcessCap::spawn_thread(
    CapRights rights, std::string_view name, memory::VirtualAddress entry, std::uint64_t arg,
    const sched::UserThreadCreateOptions& opts) const noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  SpawnSetup setup{.cspace = m_cspace.get(), .fault_endpoint = nullptr, .fault_badge = 0};
  CapRef<Endpoint> fault_endpoint;
  {
    Guard guard{m_lock};
    fault_endpoint = CapRef<Endpoint>::share(m_fault_endpoint);
    setup.fault_badge = m_fault_badge;
  }

  setup.fault_endpoint = fault_endpoint.get();
  const auto thread = m_process->spawn_thread(name, entry, arg, opts, &attach_spawned, &setup);
  if (!thread) [[unlikely]] {
    return std::unexpected(thread.error());
  }

  return std::move(setup.result);
}

std::expected<CapRef<ProcessCap>, Error> ProcessCap::fork(
    CapRights rights, std::string_view name, memory::VirtualAddress entry, std::uint64_t arg,
    const sched::UserThreadCreateOptions& opts) const noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  // The address space is copied, so it has to stay while that happens.
  const SpaceLoan loan{*m_process};
  if (loan.get() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  const auto child = m_process->fork(name, entry, arg, opts);
  if (!child) [[unlikely]] {
    return std::unexpected(child.error());
  }

  auto cap = wrap(**child);
  if (!cap) [[unlikely]] {
    (*child)->exit(proc::EXIT_FAULT);
    return cap;
  }

  Guard guard{m_lock};
  if (m_fault_endpoint != nullptr) {
    m_fault_endpoint->add_ref();
    (*cap)->m_fault_endpoint = m_fault_endpoint;
    (*cap)->m_fault_badge = m_fault_badge;
  }

  return cap;
}

std::expected<void, Error> ProcessCap::terminate(CapRights rights, int status) const noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (m_pid == proc::KERNEL_PID) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  m_process->exit(status);
  return {};
}

std::expected<proc::ProcessId, Error> ProcessCap::wait_child(CapRights rights,
                                                             proc::ProcessId target,
                                                             int* const status) const noexcept {
  if (!has_rights(rights, CapRights::Read)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  return m_process->wait_child(target, status);
}

std::expected<void, Error> ProcessCap::set_fault_endpoint(CapRights rights,
                                                          Endpoint* const endpoint,
                                                          std::uint64_t badge) noexcept {
  if (!has_rights(rights, CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (endpoint != nullptr) {
    endpoint->add_ref();
  }

  Endpoint* previous;
  {
    Guard guard{m_lock};
    previous = std::exchange(m_fault_endpoint, endpoint);
    m_fault_badge = badge;
  }

  if (previous != nullptr) {
    previous->release_ref();
  }

  return {};
}

std::expected<memory::VirtualAddress, Error> ProcessCap::allocate(
    CapRights rights, std::size_t size, memory::vmm::AccessFlags access,
    memory::vmm::PageSize page_size, memory::VirtualAddress hint) const noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const SpaceLoan loan{*m_process};
  if (loan.get() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return loan.get()->alloc(size, access, memory::vmm::CacheMode::WriteBack, page_size,
                           memory::vmm::VMAType::Anonymous, 0, hint);
}

std::expected<memory::VirtualAddress, Error> ProcessCap::map_shared(
    CapRights rights, const CSpace& source, CapId object, memory::vmm::AccessFlags access,
    std::size_t first_page, std::size_t pages, memory::VirtualAddress hint) const noexcept {
  if (!has_rights(rights, CapRights::Write)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const auto held = source.acquire<SharedVmObject>(object);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  const SpaceLoan loan{*m_process};
  if (loan.get() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return held->object->map_into(*loan.get(), held->rights, access, first_page, pages, hint);
}

std::expected<memory::VirtualAddress, Error> ProcessCap::map_mmio(
    CapRights rights, const CSpace& source, CapId region, std::size_t offset, std::size_t length,
    memory::VirtualAddress hint) const noexcept {
  if (!has_rights(rights, CapRights::Write | CapRights::Manage)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const auto held = source.acquire<MmioRegion>(region);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  const SpaceLoan loan{*m_process};
  if (loan.get() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return held->object->map_into(*loan.get(), held->rights, offset, length, true, hint);
}

std::expected<void, Error> ProcessCap::unmap(CapRights rights,
                                             memory::VirtualAddress address) const noexcept {
  if (!has_rights(rights, CapRights::Write)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  const SpaceLoan loan{*m_process};
  if (loan.get() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return loan.get()->free(address);
}

std::expected<CapId, Error> ProcessCap::grant(CapRights rights, const CSpace& source, CapId cap,
                                              CapRights mask, std::uint64_t badge) const noexcept {
  if (!has_rights(rights, CapRights::Grant)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  if (badge == 0) {
    return source.copy(cap, *m_cspace, mask);
  }

  CapRights minted;
  {
    const utils::rcu::ReadGuard rcu;
    const auto entry = source.lookup(cap, rcu);
    if (!entry) [[unlikely]] {
      return std::unexpected(entry.error());
    }

    minted = entry->rights() & mask;
  }

  return source.mint(cap, *m_cspace, minted, badge);
}
}  // namespace kernel::core::ipc
