/// @file abi.hpp
/// @brief System calls as user code sees them: their numbers, their registers, their results.
///
/// A system call is a SYSCALL instruction with the number of the call in RAX and its arguments in
/// RDI, RSI, RDX, R10, R8 and R9, in that order. The result comes back in RAX: what the call
/// returns, which is never negative, or the negated Error that stopped it. RCX and R11 are lost, as
/// SYSCALL itself uses them; every other register keeps its value unless the call says it returns
/// something there.
///
/// Calls that send a message take it in RSI (the tag) and RDX, R10, R8, R9, R12 and R13 (words 0 to
/// 5). Calls that receive one give it back in the same registers, with the badge in RDI. What does
/// not fit in registers goes through the IpcBuffer the thread names with IpcSetBuffer.
///
/// A number is a family and a function in it:
///
///     63                 12 11      8 7            0
///     [    must be zero    |  family  |   function   ]
///
/// A family is what the calls act on, and each has room to grow without moving anyone else's
/// numbers. Finding a call is one lookup for the family and one for the function, the same for
/// every number. An unhandled syscall, or bits above the family included, fails with
/// Error::NotImplemented.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/errors.hpp"

namespace kernel::core::syscall {
inline constexpr unsigned FUNCTION_BITS = 8;
inline constexpr unsigned FAMILY_BITS = 4;
inline constexpr std::uint64_t FUNCTION_MASK = (1u << FUNCTION_BITS) - 1;

enum class Family : std::uint8_t {
  Ipc = 0x0,      ///< Endpoints, notifications, and the thread's IPC buffer.
  Cap = 0x1,      ///< Capabilities and the CSpaces they are in.
  Memory = 0x2,   ///< Address spaces, and shared memory objects.
  Device = 0x3,   ///< Interrupts, MMIO, DMA for userspace drivers.
  Process = 0x4,  ///< Processes.
  Thread = 0x5,   ///< Threads, and their part in scheduling.
  Time = 0x6,     ///< The clock, and sleeping.
  Arch = 0x7,     ///< x86-64 state of the calling thread.
  Debug = 0x8,    ///< Help while there is nothing better.
};

inline constexpr std::size_t FAMILY_COUNT = 9;
static_assert(FAMILY_COUNT <= (1u << FAMILY_BITS));

[[nodiscard]] constexpr std::uint32_t make_number(const Family family,
                                                  const std::uint8_t function) noexcept {
  return (static_cast<std::uint32_t>(family) << FUNCTION_BITS) | function;
}

/// @brief Every Syscall defined by the kernel.
enum class Syscall : std::uint32_t {
  IpcCall = make_number(Family::Ipc, 0x00),
  IpcReplyRecv = make_number(Family::Ipc, 0x01),
  IpcSend = make_number(Family::Ipc, 0x02),
  IpcTrySend = make_number(Family::Ipc, 0x03),
  IpcRecv = make_number(Family::Ipc, 0x04),
  IpcTryRecv = make_number(Family::Ipc, 0x05),
  IpcReply = make_number(Family::Ipc, 0x06),
  IpcSaveReply = make_number(Family::Ipc, 0x07),
  IpcSignal = make_number(Family::Ipc, 0x08),
  IpcWait = make_number(Family::Ipc, 0x09),
  IpcPoll = make_number(Family::Ipc, 0x0A),
  IpcSetBuffer = make_number(Family::Ipc, 0x0B),

  CapCreate = make_number(Family::Cap, 0x00),
  CapMint = make_number(Family::Cap, 0x01),
  CapCopy = make_number(Family::Cap, 0x02),
  CapMove = make_number(Family::Cap, 0x03),
  CapDelete = make_number(Family::Cap, 0x04),
  CapRevoke = make_number(Family::Cap, 0x05),

  MemAllocate = make_number(Family::Memory, 0x00),
  MemProtect = make_number(Family::Memory, 0x01),
  MemUnmap = make_number(Family::Memory, 0x02),
  MemMapObject = make_number(Family::Memory, 0x03),
  MemDeriveObject = make_number(Family::Memory, 0x04),
  MemResizeObject = make_number(Family::Memory, 0x05),
  MemTranslateObject = make_number(Family::Memory, 0x06),
  MemInfo = make_number(Family::Memory, 0x07),

  DevIrqBind = make_number(Family::Device, 0x00),
  DevIrqAck = make_number(Family::Device, 0x01),
  DevMmioMap = make_number(Family::Device, 0x02),
  DevMmioDerive = make_number(Family::Device, 0x03),
  DevDmaAlloc = make_number(Family::Device, 0x04),
  DevDmaFree = make_number(Family::Device, 0x05),

  ProcCreate = make_number(Family::Process, 0x00),
  ProcFork = make_number(Family::Process, 0x01),
  ProcExit = make_number(Family::Process, 0x02),
  ProcWait = make_number(Family::Process, 0x03),
  ProcId = make_number(Family::Process, 0x04),
  ProcGrant = make_number(Family::Process, 0x05),
  ProcForkHere = make_number(Family::Process, 0x06),

  ThreadSpawn = make_number(Family::Thread, 0x00),
  ThreadExit = make_number(Family::Thread, 0x01),
  ThreadConfigure = make_number(Family::Thread, 0x02),
  ThreadYield = make_number(Family::Thread, 0x03),
  ThreadSetPriority = make_number(Family::Thread, 0x04),
  ThreadId = make_number(Family::Thread, 0x05),
  ThreadStats = make_number(Family::Thread, 0x06),

  TimeNow = make_number(Family::Time, 0x00),
  TimeSleep = make_number(Family::Time, 0x01),

  ArchSetFsBase = make_number(Family::Arch, 0x00),
  ArchGetFsBase = make_number(Family::Arch, 0x01),
  ArchSetGsBase = make_number(Family::Arch, 0x02),
  ArchGetGsBase = make_number(Family::Arch, 0x03),
  ArchEnableAmx = make_number(Family::Arch, 0x04),

  DebugLog = make_number(Family::Debug, 0x00),
};

struct MemoryInfo {
  std::uint64_t total_bytes;      ///< RAM the kernel manages, in use or not.
  std::uint64_t free_bytes;       ///< RAM free right now, about.
  std::uint64_t quota_used;       ///< What the caller's tree of processes is charged.
  std::uint64_t quota_limit;      ///< How far the tree's quota goes.
  std::uint64_t quota_available;  ///< What the caller can be charged more, right now.
};

static_assert(sizeof(MemoryInfo) == 40, "user code has the same layout");

[[nodiscard]] constexpr Family family_of(const Syscall call) noexcept {
  return static_cast<Family>(std::to_underlying(call) >> FUNCTION_BITS);
}

[[nodiscard]] constexpr std::uint8_t function_of(const Syscall call) noexcept {
  return static_cast<std::uint8_t>(std::to_underlying(call) & FUNCTION_MASK);
}

[[nodiscard]] constexpr std::int64_t encode_error(const Error error) noexcept {
  return -static_cast<std::int64_t>(std::to_underlying(error));
}

[[nodiscard]] constexpr Error decode_error(const std::int64_t rax) noexcept {
  return rax < 0 ? static_cast<Error>(-rax) : Error::Success;
}
}  // namespace kernel::core::syscall
