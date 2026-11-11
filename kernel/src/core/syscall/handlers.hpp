#pragma once

#include <cstdint>

#include "common.hpp"

namespace kernel::core::syscall::handlers {
// ipc.cpp
std::int64_t ipc_call(Frame& frame) noexcept;
std::int64_t ipc_reply_recv(Frame& frame) noexcept;
std::int64_t ipc_send(Frame& frame) noexcept;
std::int64_t ipc_try_send(Frame& frame) noexcept;
std::int64_t ipc_recv(Frame& frame) noexcept;
std::int64_t ipc_try_recv(Frame& frame) noexcept;
std::int64_t ipc_reply(Frame& frame) noexcept;
std::int64_t ipc_save_reply(Frame& frame) noexcept;
std::int64_t ipc_signal(Frame& frame) noexcept;
std::int64_t ipc_wait(Frame& frame) noexcept;
std::int64_t ipc_poll(Frame& frame) noexcept;
std::int64_t ipc_set_buffer(Frame& frame) noexcept;

// cap.cpp
std::int64_t cap_create(Frame& frame) noexcept;
std::int64_t cap_mint(Frame& frame) noexcept;
std::int64_t cap_copy(Frame& frame) noexcept;
std::int64_t cap_move(Frame& frame) noexcept;
std::int64_t cap_delete(const Frame& frame) noexcept;
std::int64_t cap_revoke(const Frame& frame) noexcept;

// memory.cpp
std::int64_t mem_allocate(Frame& frame) noexcept;
std::int64_t mem_protect(Frame& frame) noexcept;
std::int64_t mem_unmap(Frame& frame) noexcept;
std::int64_t mem_map_object(Frame& frame) noexcept;
std::int64_t mem_derive_object(Frame& frame) noexcept;
std::int64_t mem_resize_object(Frame& frame) noexcept;
std::int64_t mem_translate_object(Frame& frame) noexcept;
std::int64_t mem_info(const Frame& frame) noexcept;

// device.cpp
std::int64_t dev_irq_bind(Frame& frame) noexcept;
std::int64_t dev_irq_ack(Frame& frame) noexcept;
std::int64_t dev_mmio_map(Frame& frame) noexcept;
std::int64_t dev_mmio_derive(Frame& frame) noexcept;
std::int64_t dev_dma_alloc(Frame& frame) noexcept;
std::int64_t dev_dma_free(Frame& frame) noexcept;

// process.cpp
std::int64_t proc_create(Frame& frame) noexcept;
std::int64_t proc_fork(Frame& frame) noexcept;
std::int64_t proc_exit(Frame& frame) noexcept;
std::int64_t proc_wait(Frame& frame) noexcept;
std::int64_t proc_id(Frame& frame) noexcept;
std::int64_t proc_grant(const Frame& frame) noexcept;
std::int64_t proc_fork_here(Frame& frame) noexcept;

// thread.cpp
std::int64_t thread_spawn(Frame& frame) noexcept;
std::int64_t thread_exit(Frame& frame) noexcept;
std::int64_t thread_configure(Frame& frame) noexcept;
std::int64_t thread_yield(Frame& frame) noexcept;
std::int64_t thread_id(Frame& frame) noexcept;
std::int64_t thread_stats(Frame& frame) noexcept;

// system.cpp: the Time, Arch and Debug families.
std::int64_t time_now(Frame& frame) noexcept;
std::int64_t time_sleep(Frame& frame) noexcept;
std::int64_t arch_set_fs_base(Frame& frame) noexcept;
std::int64_t arch_get_fs_base(Frame& frame) noexcept;
std::int64_t arch_set_gs_base(Frame& frame) noexcept;
std::int64_t arch_get_gs_base(Frame& frame) noexcept;
std::int64_t arch_enable_amx(Frame& frame) noexcept;
std::int64_t debug_log(Frame& frame) noexcept;

// dispatch.cpp: what a number nothing answers to gets.
std::int64_t not_implemented(Frame& frame) noexcept;
}  // namespace kernel::core::syscall::handlers
