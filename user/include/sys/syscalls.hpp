/// @file syscalls.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <kformat.hpp>
#include <span>
#include <string_view>
#include <utility>

#include "sys/abi.hpp"
#include "sys/errors.hpp"

namespace user::sys {
// Family 0x0: IPC (`sys::ipc`)
namespace ipc {
[[nodiscard]] Result<Message> call(CapId endpoint, const Message& request) noexcept;
[[nodiscard]] Result<Message> reply_recv(CapId endpoint, const Message& reply) noexcept;
[[nodiscard]] Result<void> send(CapId endpoint, const Message& msg) noexcept;
[[nodiscard]] Result<void> try_send(CapId endpoint, const Message& msg) noexcept;
[[nodiscard]] Result<Message> recv(CapId endpoint) noexcept;
[[nodiscard]] Result<Message> try_recv(CapId endpoint) noexcept;
[[nodiscard]] Result<void> reply(const Message& msg, CapId saved_reply = NULL_CAP) noexcept;
[[nodiscard]] Result<CapId> save_reply() noexcept;
[[nodiscard]] Result<void> signal(CapId notification) noexcept;
[[nodiscard]] Result<std::uint64_t> wait(CapId notification) noexcept;
[[nodiscard]] Result<std::uint64_t> poll(CapId notification) noexcept;
/// @brief Names @p buffer as the calling thread's IPC buffer, or none with null. What the other
///        functions here stage and read is in the buffer named last.
[[nodiscard]] Result<void> set_buffer(IpcBuffer* buffer) noexcept;

/// @brief Names the calling thread's own buffer, in its thread-local storage. The runtime does
///        this for every thread it starts.
[[nodiscard]] Result<IpcBuffer*> init_default_buffer() noexcept;

/// @brief The calling thread's IPC buffer, as set_buffer() named it last; null for none.
[[nodiscard]] IpcBuffer* default_buffer() noexcept;

[[nodiscard]] Result<void> send_extended(CapId endpoint, std::uint32_t label,
                                         std::span<const std::uint64_t> words,
                                         std::span<const CapTransfer> caps = {}) noexcept;
[[nodiscard]] Result<Message> call_extended(CapId endpoint, std::uint32_t label,
                                            std::span<const std::uint64_t> words,
                                            std::span<const CapTransfer> caps = {}) noexcept;
}  // namespace ipc

// Family 0x1: Capabilities (`sys::cap`)
namespace cap {
[[nodiscard]] Result<CapId> create(CapType type, std::uint64_t size = 0,
                                   PageSize page_size = PageSize::Size4K) noexcept;
[[nodiscard]] Result<CapId> create_endpoint() noexcept;
[[nodiscard]] Result<CapId> create_notification() noexcept;
[[nodiscard]] Result<CapId> create_cspace() noexcept;
[[nodiscard]] Result<CapId> create_vmo(std::size_t size_bytes,
                                       PageSize page_size = PageSize::Size4K) noexcept;

[[nodiscard]] Result<CapId> mint(CapId src_cspace, CapId src, CapId dst_cspace, CapRights rights,
                                 std::uint64_t badge = 0, CapFlags flags = CapFlags::None) noexcept;
[[nodiscard]] Result<CapId> copy(CapId src_cspace, CapId src, CapId dst_cspace,
                                 CapRights rights_mask = CapRights::All) noexcept;
[[nodiscard]] Result<CapId> move(CapId src_cspace, CapId src, CapId dst_cspace) noexcept;
[[nodiscard]] Result<void> remove(CapId cspace, CapId cap) noexcept;
[[nodiscard]] Result<std::uint32_t> revoke(CapId cspace, CapId original) noexcept;
}  // namespace cap

// Family 0x2: Memory (`sys::mem`)
namespace mem {
[[nodiscard]] Result<void*> allocate(std::size_t size, AccessFlags access = AccessFlags::ReadWrite,
                                     PageSize page_size = PageSize::Size4K, void* hint = nullptr,
                                     CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<void> protect(void* address, std::size_t size, AccessFlags access,
                                   CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<void> unmap(void* address, CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<void*> map_object(CapId object, AccessFlags access = AccessFlags::ReadWrite,
                                       std::size_t first_page = 0, std::size_t pages = 0,
                                       void* hint = nullptr, CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<CapId> derive_object(CapId object, std::size_t first_page, std::size_t pages,
                                          CapRights rights = CapRights::All) noexcept;
[[nodiscard]] Result<void> resize_object(CapId object, std::size_t pages) noexcept;
[[nodiscard]] Result<std::uint64_t> translate_object(CapId object, std::size_t page) noexcept;
[[nodiscard]] Result<MemoryInfo> info() noexcept;
}  // namespace mem

// Family 0x3: Devices (`sys::dev`)
namespace dev {
[[nodiscard]] Result<void> irq_bind(CapId irq, CapId notification,
                                    std::uint64_t badge = 0) noexcept;
[[nodiscard]] Result<void> irq_unbind(CapId irq) noexcept;
[[nodiscard]] Result<void> irq_ack(CapId irq) noexcept;
[[nodiscard]] Result<void*> mmio_map(CapId region, std::size_t offset = 0, std::size_t length = 0,
                                     void* hint = nullptr, CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<CapId> mmio_derive(CapId region, std::size_t offset, std::size_t size,
                                        CacheMode cache = CacheMode::Uncacheable,
                                        CapRights rights = CapRights::All) noexcept;
[[nodiscard]] Result<DmaAllocation> dma_alloc(CapId pool, std::size_t size,
                                              CacheMode cache = CacheMode::Uncacheable,
                                              std::span<DmaSegment> segments = {}) noexcept;
[[nodiscard]] Result<void> dma_free(CapId pool, void* buffer) noexcept;
}  // namespace dev

// Family 0x4: Processes (`sys::proc`)
namespace proc {
[[nodiscard]] Result<CapId> create(std::string_view name, CapId parent = SELF_CAP) noexcept;
[[nodiscard]] Result<CapId> fork(std::string_view name, void (*entry)(std::uint64_t),
                                 std::uint64_t arg = 0) noexcept;
/// @brief Ends @p process with @p status. For SELF_CAP it does not come back, unless the kernel
///        refused; rt::exit() is the one that also runs exit handlers.
[[nodiscard]] Result<void> exit(int status, CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<ProcessId> wait(ProcessId child = ANY_CHILD, int* out_status = nullptr,
                                     std::uint64_t timeout_ns = 0,
                                     CapId process = SELF_CAP) noexcept;
[[nodiscard]] Result<ProcessId> id() noexcept;
[[nodiscard]] Result<CapId> grant(CapId process, CapId cap, CapRights rights_mask = CapRights::All,
                                  std::uint64_t badge = 0) noexcept;
[[nodiscard]] Result<ForkResult> fork_here(std::string_view name) noexcept;
}  // namespace proc

// Family 0x5: Threads (`sys::thread`)
namespace thread {
/// @brief Starts a thread at @p entry with @p arg, and nothing else: no thread-local storage and
///        no IPC buffer. rt::Thread is the way to start one of this process that has both.
[[nodiscard]] Result<CapId> spawn(std::string_view name, void (*entry)(std::uint64_t),
                                  std::uint64_t arg = 0, CapId process = SELF_CAP) noexcept;
/// @brief Ends @p thread. For SELF_CAP it does not come back, unless the kernel refused.
[[nodiscard]] Result<void> exit(CapId thread = SELF_CAP) noexcept;
[[nodiscard]] Result<void> configure(CapId thread_or_process, CapId notification,
                                     CapId fault_endpoint, std::uint64_t fault_badge = 0) noexcept;
void yield() noexcept;
[[nodiscard]] Result<void> set_priority() noexcept;
[[nodiscard]] ThreadId id() noexcept;
[[nodiscard]] Result<ThreadStats> stats(CapId thread = SELF_CAP) noexcept;
}  // namespace thread

// Family 0x6: Time (`sys::time`)
namespace time {
[[nodiscard]] std::uint64_t now() noexcept;
[[nodiscard]] Result<void> sleep(std::uint64_t ns, std::uint64_t slack_ns = 0,
                                 SleepFlags flags = SleepFlags::Relative) noexcept;
[[nodiscard]] Result<void> sleep_for(std::uint64_t duration_ns,
                                     std::uint64_t slack_ns = 0) noexcept;
[[nodiscard]] Result<void> sleep_until(std::uint64_t deadline_ns,
                                       std::uint64_t slack_ns = 0) noexcept;
}  // namespace time

// Family 0x7: Architecture (`sys::arch`)
namespace arch {
[[nodiscard]] Result<void> set_fs_base(void* base) noexcept;
[[nodiscard]] Result<void*> get_fs_base() noexcept;
[[nodiscard]] Result<void> set_gs_base(void* base) noexcept;
[[nodiscard]] Result<void*> get_gs_base() noexcept;
[[nodiscard]] Result<void> enable_amx() noexcept;
}  // namespace arch

// Family 0x8: Debugging (`sys::debug`)
namespace debug {
Result<void> log(std::string_view text) noexcept;

/// @note What does not fit in @see MAX_LOG_BYTES is cut off.
template <typename... Args>
void print(const klib::format_string<Args...> fmt, Args&&... args) noexcept {
  char line[MAX_LOG_BYTES + 1];
  klib::StringBuffer sink{line, sizeof(line)};
  klib::format_to(sink, fmt, std::forward<Args>(args)...);
  (void)log(sink.view());
}
}  // namespace debug
}  // namespace user::sys