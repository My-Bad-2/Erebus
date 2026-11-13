#include "sys/syscalls.hpp"

#include <algorithm>

#include "sys/invoker.hpp"

namespace user::sys {
namespace {
// The kernel knows an IPC buffer per thread (see the file comment of syscalls.hpp), so these are
// per thread too. A buffer every thread has for nothing, and the one the kernel was told about.
constinit thread_local IpcBuffer t_default_ipc_buffer{};
constinit thread_local IpcBuffer* t_ipc_buffer = nullptr;

[[nodiscard]] auto stage_message(const std::uint32_t label,
                                 const std::span<const std::uint64_t> words,
                                 const std::span<const CapTransfer> caps) noexcept
    -> Result<Message> {
  if (words.size() > MsgTag::MAX_TOTAL_WORDS || caps.size() > MsgTag::MAX_EXTRA_CAPS) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  Message msg{};
  msg.tag = MsgTag::make(label, static_cast<std::uint8_t>(words.size()),
                         static_cast<std::uint8_t>(caps.size()));

  const std::size_t inline_count = std::min<std::size_t>(words.size(), MsgTag::MAX_INLINE_WORDS);
  std::copy_n(words.begin(), inline_count, msg.words.begin());

  if (words.size() > MsgTag::MAX_INLINE_WORDS || !caps.empty()) {
    IpcBuffer* buf = t_ipc_buffer;
    if (buf == nullptr) [[unlikely]] {
      const auto init = ipc::init_default_buffer();
      if (!init) [[unlikely]] {
        return std::unexpected(init.error());
      }

      buf = *init;
    }

    std::ranges::copy(words.subspan(inline_count), buf->overflow_words.begin());
    std::ranges::copy(caps, buf->send_caps.begin());
  }

  return msg;
}
}  // namespace

namespace ipc {
Result<Message> call(const CapId endpoint, const Message& request) noexcept {
  return SyscallInvoker::invoke_ipc_call(Syscall::IpcCall, endpoint, request);
}

Result<Message> reply_recv(const CapId endpoint, const Message& reply) noexcept {
  return SyscallInvoker::invoke_ipc_call(Syscall::IpcReplyRecv, endpoint, reply);
}

Result<void> send(const CapId endpoint, const Message& msg) noexcept {
  return SyscallInvoker::invoke_ipc_send(Syscall::IpcSend, endpoint, msg);
}

Result<void> try_send(const CapId endpoint, const Message& msg) noexcept {
  return SyscallInvoker::invoke_ipc_send(Syscall::IpcTrySend, endpoint, msg);
}

Result<Message> recv(const CapId endpoint) noexcept {
  return SyscallInvoker::invoke_ipc_recv(Syscall::IpcRecv, endpoint);
}

Result<Message> try_recv(const CapId endpoint) noexcept {
  return SyscallInvoker::invoke_ipc_recv(Syscall::IpcTryRecv, endpoint);
}

Result<void> reply(const Message& msg, const CapId saved_reply) noexcept {
  return SyscallInvoker::invoke_ipc_send(Syscall::IpcReply, saved_reply, msg);
}

Result<CapId> save_reply() noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::IpcSaveReply);
}

Result<void> signal(const CapId notification) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::IpcSignal, notification);
}

Result<std::uint64_t> wait(const CapId notification) noexcept {
  return SyscallInvoker::invoke_dual(Syscall::IpcWait, notification)
      .transform([](const DualReturn& r) { return r.rdi; });
}

Result<std::uint64_t> poll(const CapId notification) noexcept {
  return SyscallInvoker::invoke_dual(Syscall::IpcPoll, notification)
      .transform([](const DualReturn& r) { return r.rdi; });
}

Result<void> set_buffer(IpcBuffer* const buffer) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::IpcSetBuffer, buffer).transform([buffer] {
    t_ipc_buffer = buffer;
  });
}

Result<IpcBuffer*> init_default_buffer() noexcept {
  IpcBuffer* const buffer = &t_default_ipc_buffer;
  return set_buffer(buffer).transform([buffer] { return buffer; });
}

IpcBuffer* default_buffer() noexcept { return t_ipc_buffer; }

Result<void> send_extended(const CapId endpoint, const std::uint32_t label,
                           const std::span<const std::uint64_t> words,
                           const std::span<const CapTransfer> caps) noexcept {
  const auto staged = stage_message(label, words, caps);
  if (!staged) [[unlikely]] {
    return std::unexpected(staged.error());
  }

  return send(endpoint, *staged);
}

Result<Message> call_extended(const CapId endpoint, const std::uint32_t label,
                              const std::span<const std::uint64_t> words,
                              const std::span<const CapTransfer> caps) noexcept {
  const auto staged = stage_message(label, words, caps);
  if (!staged) [[unlikely]] {
    return std::unexpected(staged.error());
  }

  return call(endpoint, *staged);
}
}  // namespace ipc

namespace cap {
Result<CapId> create(const CapType type, const std::uint64_t size,
                     const PageSize page_size) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::CapCreate, type, size, page_size);
}

Result<CapId> create_endpoint() noexcept { return create(CapType::Endpoint); }

Result<CapId> create_notification() noexcept { return create(CapType::Notification); }

Result<CapId> create_cspace() noexcept { return create(CapType::CSpace); }

Result<CapId> create_vmo(const std::size_t size_bytes, const PageSize page_size) noexcept {
  return create(CapType::VmObject, size_bytes, page_size);
}

Result<CapId> mint(const CapId src_cspace, const CapId src, const CapId dst_cspace,
                   const CapRights rights, const std::uint64_t badge,
                   const CapFlags flags) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::CapMint, src_cspace, src, dst_cspace, rights,
                                          badge, flags);
}

Result<CapId> copy(const CapId src_cspace, const CapId src, const CapId dst_cspace,
                   const CapRights rights_mask) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::CapCopy, src_cspace, src, dst_cspace,
                                          rights_mask);
}

Result<CapId> move(const CapId src_cspace, const CapId src, const CapId dst_cspace) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::CapMove, src_cspace, src, dst_cspace);
}

Result<void> remove(const CapId cspace, const CapId cap) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::CapDelete, cspace, cap);
}

Result<std::uint32_t> revoke(const CapId cspace, const CapId original) noexcept {
  return SyscallInvoker::invoke_as<std::uint32_t>(Syscall::CapRevoke, cspace, original);
}
}  // namespace cap

namespace mem {
Result<void*> allocate(const std::size_t size, const AccessFlags access, const PageSize page_size,
                       void* const hint, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void*>(Syscall::MemAllocate, process, size, access, page_size,
                                          hint);
}

Result<void> protect(void* const address, const std::size_t size, const AccessFlags access,
                     const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::MemProtect, process, address, size, access);
}

Result<void> unmap(void* const address, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::MemUnmap, process, address);
}

Result<void*> map_object(const CapId object, const AccessFlags access, const std::size_t first_page,
                         const std::size_t pages, void* const hint, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void*>(Syscall::MemMapObject, process, object, access,
                                          first_page, pages, hint);
}

Result<CapId> derive_object(const CapId object, const std::size_t first_page,
                            const std::size_t pages, const CapRights rights) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::MemDeriveObject, object, first_page, pages,
                                          rights);
}

Result<void> resize_object(const CapId object, const std::size_t pages) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::MemResizeObject, object, pages);
}

Result<std::uint64_t> translate_object(const CapId object, const std::size_t page) noexcept {
  return SyscallInvoker::invoke_as<std::uint64_t>(Syscall::MemTranslateObject, object, page);
}

Result<MemoryInfo> info() noexcept {
  MemoryInfo out{};
  return SyscallInvoker::invoke_as<void>(Syscall::MemInfo, &out).transform([&] { return out; });
}
}  // namespace mem

namespace dev {
Result<void> irq_bind(const CapId irq, const CapId notification,
                      const std::uint64_t badge) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::DevIrqBind, irq, notification, badge);
}

Result<void> irq_unbind(const CapId irq) noexcept { return irq_bind(irq, NULL_CAP, 0); }

Result<void> irq_ack(const CapId irq) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::DevIrqAck, irq);
}

Result<void*> mmio_map(const CapId region, const std::size_t offset, const std::size_t length,
                       void* const hint, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void*>(Syscall::DevMmioMap, process, region, offset, length,
                                          hint);
}

Result<CapId> mmio_derive(const CapId region, const std::size_t offset, const std::size_t size,
                          const CacheMode cache, const CapRights rights) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::DevMmioDerive, region, offset, size, cache,
                                          rights);
}

Result<DmaAllocation> dma_alloc(const CapId pool, const std::size_t size, const CacheMode cache,
                                const std::span<DmaSegment> segments) noexcept {
  return SyscallInvoker::invoke_dual(Syscall::DevDmaAlloc, pool, size, cache, segments.data(),
                                     segments.size())
      .transform([](const DualReturn& r) {
        return DmaAllocation{
            .virt_addr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(r.rax)),
            .first_phys_addr = r.rdi,
        };
      });
}

Result<void> dma_free(const CapId pool, void* const buffer) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::DevDmaFree, pool, buffer);
}
}  // namespace dev

namespace proc {
Result<CapId> create(const std::string_view name, const CapId parent) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::ProcCreate, name.data(), name.size(), parent);
}

Result<CapId> fork(const std::string_view name, void (*const entry)(std::uint64_t),
                   const std::uint64_t arg) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::ProcFork, name.data(), name.size(), entry, arg);
}

Result<void> exit(const int status, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ProcExit, process, status);
}

Result<ProcessId> wait(const ProcessId child, int* const out_status, const std::uint64_t timeout_ns,
                       const CapId process) noexcept {
  return SyscallInvoker::invoke_as<ProcessId>(Syscall::ProcWait, process, child, timeout_ns,
                                              out_status);
}

Result<ProcessId> id() noexcept { return SyscallInvoker::invoke_as<ProcessId>(Syscall::ProcId); }

Result<CapId> grant(const CapId process, const CapId cap, const CapRights rights_mask,
                    const std::uint64_t badge) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::ProcGrant, process, cap, rights_mask, badge);
}

Result<ForkResult> fork_here(const std::string_view name) noexcept {
  return SyscallInvoker::invoke_dual(Syscall::ProcForkHere, name.data(), name.size())
      .transform([](const DualReturn& r) {
        return ForkResult{
            .child_process = CapId{static_cast<std::uint32_t>(r.rax)},
            .child_pid = static_cast<ProcessId>(r.rdi),
        };
      });
}
}  // namespace proc

namespace thread {
Result<CapId> spawn(const std::string_view name, void (*const entry)(std::uint64_t),
                    const std::uint64_t arg, const CapId process) noexcept {
  return SyscallInvoker::invoke_as<CapId>(Syscall::ThreadSpawn, process, name.data(), name.size(),
                                          entry, arg);
}

Result<void> exit(const CapId thread) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ThreadExit, thread);
}

Result<void> configure(const CapId thread_or_process, const CapId notification,
                       const CapId fault_endpoint, const std::uint64_t fault_badge) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ThreadConfigure, thread_or_process, notification,
                                         fault_endpoint, fault_badge);
}

void yield() noexcept { (void)SyscallInvoker::invoke_raw(Syscall::ThreadYield); }

Result<void> set_priority() noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ThreadSetPriority);
}

ThreadId id() noexcept {
  return static_cast<ThreadId>(SyscallInvoker::invoke_raw(Syscall::ThreadId));
}

Result<ThreadStats> stats(const CapId thread) noexcept {
  ThreadStats out{};
  return SyscallInvoker::invoke_as<void>(Syscall::ThreadStats, thread, &out).transform([&] {
    return out;
  });
}
}  // namespace thread

namespace time {
std::uint64_t now() noexcept {
  return static_cast<std::uint64_t>(SyscallInvoker::invoke_raw(Syscall::TimeNow));
}

Result<void> sleep(const std::uint64_t ns, const std::uint64_t slack_ns,
                   const SleepFlags flags) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::TimeSleep, ns, slack_ns, flags);
}

Result<void> sleep_for(const std::uint64_t duration_ns, const std::uint64_t slack_ns) noexcept {
  return sleep(duration_ns, slack_ns, SleepFlags::Relative);
}

Result<void> sleep_until(const std::uint64_t deadline_ns, const std::uint64_t slack_ns) noexcept {
  return sleep(deadline_ns, slack_ns, SleepFlags::Absolute);
}
}  // namespace time

namespace arch {
Result<void> set_fs_base(void* const base) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ArchSetFsBase, base);
}

Result<void*> get_fs_base() noexcept {
  return SyscallInvoker::invoke_as<void*>(Syscall::ArchGetFsBase);
}

Result<void> set_gs_base(void* const base) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ArchSetGsBase, base);
}

Result<void*> get_gs_base() noexcept {
  return SyscallInvoker::invoke_as<void*>(Syscall::ArchGetGsBase);
}

Result<void> enable_amx() noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::ArchEnableAmx);
}
}  // namespace arch

namespace debug {
Result<void> log(const std::string_view text) noexcept {
  return SyscallInvoker::invoke_as<void>(Syscall::DebugLog, text.data(), text.size());
}
}  // namespace debug
}  // namespace user::sys