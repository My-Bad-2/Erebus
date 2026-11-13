/// @file runtime.hpp
/// @brief What runs around a program: how it starts, how it ends, and the C and C++ support code
///        that the compiler and klib expect to find.
///
/// The kernel starts a program's first thread at `_start` with a stack, a zero return address on
/// it, and nothing else: no thread-local storage, no IPC buffer, constructors not run. The runtime
/// (src/rt/start.cpp) puts all of that in place and then calls user::main(), whose result becomes
/// the exit status:
///
///  1. Thread-local storage, as static executables have it: a copy of the image's TLS template (the
///  PT_TLS segment), and right after it the thread control block, where FS points. The block's
///  first word points at itself, because compiled code reads %fs:0 to find thread-local variables.
///  2. The thread's IPC buffer, which lives in that storage (see sys/syscalls.hpp).
///  3. .preinit_array and .init_array, in order.
///
/// Threads started with rt::Thread get the first two of these as well. exit() runs what atexit()
/// registered, the other way round, then .fini_array, then ends the process.
#pragma once

namespace user {
int main() noexcept;
}  // namespace user

namespace user::rt {
inline constexpr int EXIT_ABORTED = -2;
inline constexpr int EXIT_NO_RUNTIME = -3;

[[noreturn]] void exit(int status) noexcept;
[[noreturn]] void abort() noexcept;
}  // namespace user::rt

extern "C" {
int atexit(void (*handler)()) noexcept;
[[noreturn]] void abort() noexcept;
}
