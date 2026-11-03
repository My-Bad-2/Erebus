#include "utils/logger.hpp"

extern "C" {
[[noreturn]] void abort() noexcept { kernel::utils::logger::fatal("abort() called.\n"); }

[[noreturn]] void __assert_fail(const char* assertion, const char* file, const unsigned int line,
                                const char* function) noexcept {
  kernel::utils::logger::fatal("Assertion `{}` failed at {}:{} in {}.\n", assertion, file, line,
                               function);
}

int atexit(void (*)()) noexcept { return 0; }

[[noreturn]] void __cxa_pure_virtual() noexcept {
  kernel::utils::logger::fatal("Pure virtual function called.\n");
}
}

void std::terminate() noexcept { kernel::utils::logger::fatal("std::terminate() called.\n"); }
