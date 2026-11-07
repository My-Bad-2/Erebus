#include "utils/logger.hpp"

using namespace kernel::utils;
extern "C" {
[[noreturn]] void abort() noexcept { logger::fatal("abort() called.\n"); }

[[noreturn]] void __assert_fail(const char* assertion, const char* file, const unsigned int line,
                                const char* function) noexcept {
  logger::fatal("Assertion `{}` failed at {}:{} in {}.\n", assertion, file, line, function);
}

int atexit(void (*)()) noexcept { return 0; }

[[noreturn]] void __cxa_pure_virtual() noexcept {
  logger::fatal("Pure virtual function called.\n");
}
}

void std::terminate() noexcept { logger::fatal("std::terminate() called.\n"); }
