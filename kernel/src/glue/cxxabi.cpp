#include "utils/logger.hpp"

extern "C" {
[[noreturn]] void abort() {
  kernel::utils::logger::fatal("Abort Called!\n");
  std::unreachable();
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
  kernel::utils::logger::error("{} failed @ {}:{}:{}!", assertion, file, line, function);
}
}