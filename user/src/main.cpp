#include "rt/runtime.hpp"
#include "sys/syscalls.hpp"

#ifdef EREBUS_USER_TESTS
#include "ktest.hpp"
#endif

namespace user {
int main() noexcept {
  sys::debug::print("Hello, World!\n");

  if (const auto info = sys::mem::info()) {
    const auto [total_bytes, free_bytes, quota_used, quota_limit, quota_available] = *info;
    sys::debug::print("total_memory    = {} MiB\n", total_bytes / 1024 / 1024);
    sys::debug::print("free_memory     = {} MiB\n", free_bytes / 1024 / 1024);
    sys::debug::print("quota_used      = {} MiB\n", quota_used / 1024 / 1024);
    sys::debug::print("quota_limit     = {} MiB\n", quota_limit / 1024 / 1024);
    sys::debug::print("quota_available = {} MiB\n", quota_available / 1024 / 1024);
  } else {
    sys::debug::print("mem_info failed: {}\n", sys::to_string(info.error()));
  }

#ifdef EREBUS_USER_TESTS
  // The test image: what the kernel's test run waits for. Its exit status is the verdict.
  return ktest::run_all();
#else
  return 0;
#endif
}
}  // namespace user
