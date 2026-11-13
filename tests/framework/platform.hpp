// Between the runner's shared part (ktest.cpp) and the place it runs in: kernel.cpp for the test
// kernel, user.cpp for the user test image. Each image links ktest.cpp and one of those two.
#pragma once

#include <cstdint>
#include <kformat.hpp>
#include <span>
#include <string_view>

#include "ktest.hpp"

// What the place supplies.
namespace ktest::platform {
/// The first word of every line the runner prints: "ktest" in the kernel, "utest" in user space,
/// so that a log says which side wrote what.
[[nodiscard]] std::string_view name() noexcept;

/// What a participant of a KTEST_ALL_CPUS() test is called in a failure: "CPU" or "thread".
[[nodiscard]] std::string_view participant_kind() noexcept;

/// Which participant the caller is. Only asked while a test runs on more than one.
[[nodiscard]] std::uint32_t participant() noexcept;

/// Writes @p line, which ends in a newline, to the console in one piece.
void write(std::string_view line) noexcept;

/// Nanoseconds since some fixed point, for how long a test took.
[[nodiscard]] std::uint64_t now_ns() noexcept;

/// Runs @p test's body on every participant at once, and returns once each is done with it.
void run_everywhere(const detail::TestCase& test) noexcept;
}  // namespace ktest::platform

// What the shared part gives each place.
namespace ktest::detail {
/// Every test registered in this image.
[[nodiscard]] std::span<const TestCase> registered_tests() noexcept;

/// Runs, one after another, every registered test of @p order, and prints a line for each.
[[nodiscard]] Totals run_tests(Order order) noexcept;

/// Runs one test that is not registered, but made up by the place, the user image's run say:
/// @p body fails or skips it the way a test body would.
[[nodiscard]] Totals run_one(const TestCase& test) noexcept;

/// Prints the totals, then the verdict: ALL PASSED or FAILED.
void report(const Totals& totals) noexcept;

/// Formats a line, with the place's name and a colon first, and writes it. A line too long for
/// the buffer still ends in a newline, in place of its last character.
template <typename... Args>
void print(const klib::format_string<Args...> fmt, Args&&... args) noexcept {
  char line[512];
  klib::StringBuffer sink{line, sizeof(line)};
  sink.append(platform::name());
  sink.append(": ");
  klib::format_to(sink, fmt, std::forward<Args>(args)...);

  const std::span<char> text = sink.active_span();
  if (!text.empty() && text.back() != '\n') {
    if (const std::span<char> slot = sink.advance(1); !slot.empty()) {
      slot.front() = '\n';
    } else {
      text.back() = '\n';
    }
  }

  platform::write(sink.view());
}
}  // namespace ktest::detail
