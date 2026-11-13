// Thread-local storage blocks: the runtime's private part of rt/runtime.hpp.
//
// One block per thread, from sys::mem::allocate(), laid out as x86-64 variant II wants it:
//
//     base                                    tp = FS base
//     | TLS template copy, zero-filled to the | Tcb ... |
//     |  template's alignment                 |
//
// The linker resolves a thread-local variable to a fixed, negative offset from tp: minus the
// template's size rounded up to its alignment, plus the variable's place in it. So the copy ends
// exactly at tp, and tp is aligned like the template, which a page-aligned base gives for any
// alignment up to a page, and like the Tcb.
#pragma once

#include <cstddef>

#include "rt/thread.hpp"
#include "sys/abi.hpp"
#include "sys/errors.hpp"

namespace user::rt::tls {
struct Tcb {
  Tcb* self;                ///< %%fs:0 base
  void* block;              ///< The block's base, which goes back to the kernel with destroy().
  std::size_t block_bytes;  ///< And its size.

  Thread::Entry entry;
  void* context;
  sys::CapId thread;   ///< The thread's capability.
  sys::CapId done;     ///< The notification the thread signals when the entry has returned.
  sys::Error failure;  ///< Why the entry never ran, or Success.
};

/// @brief A new block thread block.
[[nodiscard]] sys::Result<Tcb*> create() noexcept;

/// @brief Gives the block back. @pre No thread uses it anymore, or will.
void destroy(Tcb* tcb) noexcept;

/// @brief Points the calling thread's FS at @p tcb, which makes the block its storage.
[[nodiscard]] sys::Result<void> install(Tcb* tcb) noexcept;
}  // namespace user::rt::tls
