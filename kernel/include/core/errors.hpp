#pragma once

#include <cstdint>

namespace kernel {
enum class Error : std::uint32_t {
  Success = 0x0,

  // Generic errors
  NotImplemented = 0x1001,
  InvalidState = 0x1002,
  InvalidFlags = 0x1003,
  InvalidArgument = 0x1004,
  Timeout = 0x1005,
  NotFound = 0x1006,

  // VMM and Heap Allocator
  OutOfMemory = 0x2001,
  OutOfBounds = 0x2002,
  InvalidAlignment = 0x2003,
  AlreadyMapped = 0x2004,
  NotMapped = 0x2005,
  StackOverflow = 0x2006,
  PageFault = 0x2007,
  MemoryProtectionFault = 0x2008,

  // Synchronization
  CasFailed = 0x3001,
  DeadlockDetected = 0x3002,
  WouldBlock = 0x3003,

  // Security & Permissions
  SecurityViolation = 0x4001,
  AccessDenied = 0x4002,
  IBTViolation = 0x4003,
  PrivilegeEscalationDetected = 0x4004,

  // Kernel Patcher & Insn Decoding
  InstructionBoundaryMismatch = 0x5001,
  TargetTooSmallForTrampoline = 0x5002,
  ZydisRelocationFailed = 0x5003,
  ZydisConversionFailed = 0x5004,
  InvalidOpcode = 0x5005,
  TrampolineAllocationFailed = 0x5006,
};

[[nodiscard]] constexpr bool IsSuccess(const Error e) noexcept { return e == Error::Success; }
[[nodiscard]] constexpr bool IsError(const Error e) noexcept { return e != Error::Success; }
[[nodiscard]] constexpr uint32_t GetErrorSubsystem(const Error e) noexcept { return static_cast<uint32_t>(e) & 0xF000; }
} // namespace kernel