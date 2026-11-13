/// @file errors.hpp
/// @brief What a failed system call says, and the Result every wrapper in sys/ returns.
///
/// The codes are the kernel's own (kernel/include/core/errors.hpp), so that a value in RAX means
/// the same on both sides. tests/kernel/core/user_abi.cpp fails the build if the two lists drift.
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace user::sys {
enum class Error : std::uint32_t {
  Success = 0x0,
  HardwareError = 0x1,

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
  QuotaExceeded = 0x2009,

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
  SiteOutOfTextBounds = 0x5007,
  ZydisDecodeFailed = 0x5008,
  ZydisEncodeFailed = 0x5009,
  UnexpectedSiteMnemonic = 0x500A,

  // IOAPIC errors
  UnmappedGSI = 0x6001,
  ControllerLimitReached = 0x6002,
  OverrideLimitReached = 0x6003,
};

template <typename T = void>
using Result = std::expected<T, Error>;

[[nodiscard]] constexpr std::int64_t encode_error(const Error error) noexcept {
  return -static_cast<std::int64_t>(std::to_underlying(error));
}

[[nodiscard]] constexpr Error decode_error(const std::int64_t rax) noexcept {
  return rax < 0 ? static_cast<Error>(-rax) : Error::Success;
}

/// @brief The name of @p error, for messages; "?" for a value that is none of them.
[[nodiscard]] constexpr std::string_view to_string(const Error error) noexcept {
  switch (error) {
    case Error::Success:
      return "Success";
    case Error::HardwareError:
      return "HardwareError";
    case Error::NotImplemented:
      return "NotImplemented";
    case Error::InvalidState:
      return "InvalidState";
    case Error::InvalidFlags:
      return "InvalidFlags";
    case Error::InvalidArgument:
      return "InvalidArgument";
    case Error::Timeout:
      return "Timeout";
    case Error::NotFound:
      return "NotFound";
    case Error::OutOfMemory:
      return "OutOfMemory";
    case Error::OutOfBounds:
      return "OutOfBounds";
    case Error::InvalidAlignment:
      return "InvalidAlignment";
    case Error::AlreadyMapped:
      return "AlreadyMapped";
    case Error::NotMapped:
      return "NotMapped";
    case Error::StackOverflow:
      return "StackOverflow";
    case Error::PageFault:
      return "PageFault";
    case Error::MemoryProtectionFault:
      return "MemoryProtectionFault";
    case Error::QuotaExceeded:
      return "QuotaExceeded";
    case Error::CasFailed:
      return "CasFailed";
    case Error::DeadlockDetected:
      return "DeadlockDetected";
    case Error::WouldBlock:
      return "WouldBlock";
    case Error::SecurityViolation:
      return "SecurityViolation";
    case Error::AccessDenied:
      return "AccessDenied";
    case Error::IBTViolation:
      return "IBTViolation";
    case Error::PrivilegeEscalationDetected:
      return "PrivilegeEscalationDetected";
    case Error::InstructionBoundaryMismatch:
      return "InstructionBoundaryMismatch";
    case Error::TargetTooSmallForTrampoline:
      return "TargetTooSmallForTrampoline";
    case Error::ZydisRelocationFailed:
      return "ZydisRelocationFailed";
    case Error::ZydisConversionFailed:
      return "ZydisConversionFailed";
    case Error::InvalidOpcode:
      return "InvalidOpcode";
    case Error::TrampolineAllocationFailed:
      return "TrampolineAllocationFailed";
    case Error::SiteOutOfTextBounds:
      return "SiteOutOfTextBounds";
    case Error::ZydisDecodeFailed:
      return "ZydisDecodeFailed";
    case Error::ZydisEncodeFailed:
      return "ZydisEncodeFailed";
    case Error::UnexpectedSiteMnemonic:
      return "UnexpectedSiteMnemonic";
    case Error::UnmappedGSI:
      return "UnmappedGSI";
    case Error::ControllerLimitReached:
      return "ControllerLimitReached";
    case Error::OverrideLimitReached:
      return "OverrideLimitReached";
  }

  return "?";
}
}  // namespace user::sys
