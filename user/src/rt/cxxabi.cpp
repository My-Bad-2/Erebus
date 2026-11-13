#include <exception>

#include "rt/runtime.hpp"
#include "sys/syscalls.hpp"

extern "C" {
void abort() noexcept { user::rt::abort(); }

[[noreturn]] void __assert_fail(const char* const assertion, const char* const file,
                                const unsigned int line, const char* const function) noexcept {
  user::sys::debug::print("Assertion `{}` failed at {}:{} in {}.", assertion, file, line, function);
  user::rt::abort();
}

[[noreturn]] void __cxa_pure_virtual() noexcept {
  user::sys::debug::print("Pure virtual function called.");
  user::rt::abort();
}
}

void std::terminate() noexcept {
  user::sys::debug::print("std::terminate() called.");
  user::rt::abort();
}
