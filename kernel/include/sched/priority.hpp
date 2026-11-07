/// @file priority.hpp
/// @brief How much a thread counts for: what it is, and how large its share is.
#pragma once

#include <compare>
#include <cstdint>

namespace kernel::sched {
/// @brief Scheduling priority classes used to parameterize thread weight and latency targets
enum class PriorityClass : std::uint8_t {
  Idle = 0,        ///< Per-CPU idle thread
  Background = 1,  ///< Low-Priority background maintenance / compaction daemons
  Normal = 2,      ///< Standard Kernel and User Threads (EEVDF baseline weight = 1024)
  High = 3,        ///< Latency-sensitive kernel worker / bottom-half threads
  RealTime = 4,    ///< Fixed-priority / immediate preemption threads
  TestRunner = 5,  ///< Pinned in-kernel ktest runner threads
};

/// @brief Returns the canonical EEVDF scheduling weight corresponding to @p prio.
[[nodiscard]] constexpr std::uint32_t priority_to_weight(const PriorityClass prio) noexcept {
  switch (prio) {
    case PriorityClass::Idle:
      return 1;
    case PriorityClass::Background:
      return 256;
    case PriorityClass::Normal:
      return 1024;
    case PriorityClass::High:
      return 4096;
    case PriorityClass::RealTime:
      return 32768;
    case PriorityClass::TestRunner:
      return 65536;
  }

  return 1024;
}

/// @brief What a thread competes with: the class picks the tier, the weight the share in it.
///
/// Compares the way the scheduler would rank two threads, so priority inheritance comes down to
/// taking a maximum.
struct Priority {
  PriorityClass priority_class{PriorityClass::Normal};
  std::uint32_t weight{priority_to_weight(PriorityClass::Normal)};

  /// @brief The usual priority of a thread of class @p prio.
  [[nodiscard]] static constexpr Priority of(const PriorityClass prio) noexcept {
    return {.priority_class = prio, .weight = priority_to_weight(prio)};
  }

  [[nodiscard]] constexpr auto operator<=>(const Priority&) const noexcept = default;
};
}  // namespace kernel::sched
