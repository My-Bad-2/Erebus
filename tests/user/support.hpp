// What the user image's tests share: checks on a sys::Result that say which error came, and a
// way to run something in a process of its own.
#pragma once

#include <source_location>
#include <string_view>
#include <utility>

#include "ktest.hpp"
#include "sys/errors.hpp"
#include "sys/syscalls.hpp"

/// @brief Fails the test unless `result` holds a value, and says which error it holds if not.
#define KEXPECT_OK(result) ::user::testing::expect_ok((result), #result, KTEST_HERE)

/// @brief KEXPECT_OK(), then out of the test body. The check is spelled out here, so that
///        static analysis sees `*result` is safe after it.
#define KREQUIRE_OK(result)                                      \
  do {                                                           \
    if (!(result).has_value()) {                                 \
      ::user::testing::expect_ok((result), #result, KTEST_HERE); \
      return;                                                    \
    }                                                            \
  } while (false)

/// @brief Fails the test unless `result` failed, with `error`.
#define KEXPECT_ERROR(result, error) \
  ::user::testing::expect_error((result), (error), #result, KTEST_HERE)

namespace user::testing {
template <typename T>
bool expect_ok(const sys::Result<T>& result, const char* const expr,
               const std::source_location where) noexcept {
  if (!result) [[unlikely]] {
    ktest::detail::fail_with(where, "expected {} to succeed, but it failed with {}", expr,
                             sys::to_string(result.error()));
    return false;
  }

  return true;
}

template <typename T>
bool expect_error(const sys::Result<T>& result, const sys::Error error, const char* const expr,
                  const std::source_location where) noexcept {
  if (result) [[unlikely]] {
    ktest::detail::fail_with(where, "expected {} to fail with {}, but it succeeded", expr,
                             sys::to_string(error));
    return false;
  }

  if (result.error() != error) [[unlikely]] {
    ktest::detail::fail_with(where, "expected {} to fail with {}, but it failed with {}", expr,
                             sys::to_string(error), sys::to_string(result.error()));
    return false;
  }

  return true;
}

/// @brief Runs @p body in a child that ProcForkHere makes, and waits for it.
///
/// The child is a copy of this process with this thread alone, so @p body sees everything as it
/// was, capabilities included. It reports by what it returns, which becomes its exit status:
/// what KEXPECT_*() in it count is the child's, and goes with it. It ends without running exit
/// handlers, as _exit() would after a fork().
/// @return The child's exit status.
template <typename Body>
[[nodiscard]] sys::Result<int> in_child(const std::string_view name, Body&& body) noexcept {
  const auto forked = sys::proc::fork_here(name);
  if (!forked) {
    return std::unexpected(forked.error());
  }

  if (forked->is_child()) {
    (void)sys::proc::exit(std::forward<Body>(body)());
    __builtin_trap();
  }

  int status = 0;
  const auto waited = sys::proc::wait(forked->child_pid, &status);
  (void)sys::cap::remove(sys::NULL_CAP, forked->child_process);
  if (!waited) {
    return std::unexpected(waited.error());
  }

  return status;
}
}  // namespace user::testing
