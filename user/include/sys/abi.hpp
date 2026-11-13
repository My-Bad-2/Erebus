/// @file abi.hpp
/// @brief The system call ABI as user code sees it: numbers, the types that cross into the kernel,
///        and their layouts.
///
/// misc/syscalls.md says what each call does and which register takes what. Everything here has
/// to match the kernel's side (kernel/include/core/syscall/abi.hpp, core/cap.hpp,
/// core/ipc/message.hpp and friends) bit for bit. tests/kernel/core/user_abi.cpp compares the two
/// at compile time, so a change on one side alone fails the build.
#pragma once

#include <array>
#include <bitfield.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sys/errors.hpp"

namespace user::sys {
inline constexpr std::size_t MAX_LOG_BYTES = 512;

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

// Identifiers and Capability Types

using ProcessId = std::uint32_t;
using ThreadId = std::uint64_t;

inline constexpr ProcessId ANY_CHILD = ~0u;

class CapId {
  std::uint32_t m_data{0};

 public:
  static constexpr std::uint32_t INDEX_BITS = 20;
  static constexpr std::uint32_t GEN_BITS = 12;

  constexpr CapId() noexcept = default;
  constexpr explicit CapId(const std::uint32_t raw) noexcept : m_data(raw) {}

  [[nodiscard]] static constexpr CapId make(const std::uint32_t index,
                                            const std::uint16_t generation) noexcept {
    CapId id{};
    id.set_index(index);
    id.set_generation(generation);
    return id;
  }

  [[nodiscard]] static constexpr CapId null() noexcept { return CapId{0}; }
  [[nodiscard]] static constexpr CapId self() noexcept { return CapId{0}; }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return m_data; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return m_data == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_data != 0; }

  BF_RW(std::uint32_t, index, 0, INDEX_BITS)
  BF_RW(std::uint16_t, generation, INDEX_BITS, GEN_BITS)

  [[nodiscard]] constexpr bool operator==(const CapId&) const noexcept = default;
};

inline constexpr CapId NULL_CAP = CapId::null();
inline constexpr CapId SELF_CAP = CapId::self();

enum class CapType : std::uint8_t {
  Null = 0,
  Endpoint = 1,
  Notification = 2,
  Reply = 3,
  VmObject = 4,
  Mmio = 5,
  IrqHandler = 6,
  DmaPool = 7,
  CSpace = 8,
  Thread = 9,
  Process = 10,
};

enum class CapRights : std::uint8_t {
  None = 0,
  Read = 1 << 0,        ///< Receive on endpoint, wait on notification, map read-only.
  Write = 1 << 1,       ///< Send/call on endpoint, signal notification, map writable.
  Grant = 1 << 2,       ///< Transfer capabilities in IPC messages or `proc_grant`.
  GrantReply = 1 << 3,  ///< Allow call reply to carry capabilities back.
  Mint = 1 << 4,        ///< Badge a copy or derive a sub-object.
  Revoke = 1 << 5,      ///< Revoke derived capabilities from an original.
  Manage = 1 << 6,      ///< Configure object (bind, ack, spawn, terminate, physical address).
  Execute = 1 << 7,     ///< Map executable.

  All = 0xFF,
  ReadWrite = Read | Write,
  ClientCall = Write | GrantReply,
  ServerEndpoint = Read | Write | Grant | Mint | Revoke | Manage,
  DriverFull = Read | Write | Mint | Revoke | Manage,
};

[[nodiscard]] constexpr CapRights operator|(const CapRights a, const CapRights b) noexcept {
  return static_cast<CapRights>(std::to_underlying(a) | std::to_underlying(b));
}
[[nodiscard]] constexpr CapRights operator&(const CapRights a, const CapRights b) noexcept {
  return static_cast<CapRights>(std::to_underlying(a) & std::to_underlying(b));
}
[[nodiscard]] constexpr CapRights operator~(const CapRights a) noexcept {
  return static_cast<CapRights>(~std::to_underlying(a));
}

enum class CapFlags : std::uint8_t {
  None = 0,
  OneShot = 1 << 0,
  Badged = 1 << 1,
  Pinned = 1 << 2,
  Original = 1 << 3,
};

[[nodiscard]] constexpr CapFlags operator|(const CapFlags a, const CapFlags b) noexcept {
  return static_cast<CapFlags>(std::to_underlying(a) | std::to_underlying(b));
}

[[nodiscard]] constexpr CapFlags operator&(const CapFlags a, const CapFlags b) noexcept {
  return static_cast<CapFlags>(std::to_underlying(a) & std::to_underlying(b));
}

// Memory & Device Flags

enum class AccessFlags : std::uint16_t {
  None = 0,
  Read = 0x0001,
  Write = 0x0002,
  Execute = 0x0004,
  Stack = 0x0800,     ///< Grows downward on page fault.
  Populate = 0x1000,  ///< Fault in backing pages immediately.

  ReadWrite = Read | Write,
  ReadExecute = Read | Execute,
};

[[nodiscard]] constexpr AccessFlags operator|(const AccessFlags a, const AccessFlags b) noexcept {
  return static_cast<AccessFlags>(std::to_underlying(a) | std::to_underlying(b));
}
[[nodiscard]] constexpr AccessFlags operator&(const AccessFlags a, const AccessFlags b) noexcept {
  return static_cast<AccessFlags>(std::to_underlying(a) & std::to_underlying(b));
}

enum class PageSize : std::uint8_t {
  Default = 0,  ///< Treated as 4 KiB by the kernel.
  Size4K = 1,
  Size2M = 2,
  Size1G = 3,
};

enum class CacheMode : std::uint8_t {
  WriteBack = 0,
  WriteThrough = 1,
  UncacheableMinus = 2,
  Uncacheable = 3,
  WriteCombining = 4,
  WriteProtected = 5,
};

enum class SleepFlags : std::uint64_t {
  Relative = 0,
  Absolute = 1 << 0,
};

// IPC Message & Buffer Layout

enum class MsgFlags : std::uint8_t {
  None = 0,
  IsAsyncNotification = 1 << 0,
  DonateSchedContext = 1 << 1,
  KernelError = 1 << 2,
  ReplyRequired = 1 << 3,
};

[[nodiscard]] constexpr MsgFlags operator|(const MsgFlags a, const MsgFlags b) noexcept {
  return static_cast<MsgFlags>(std::to_underlying(a) | std::to_underlying(b));
}

[[nodiscard]] constexpr MsgFlags operator&(const MsgFlags a, const MsgFlags b) noexcept {
  return static_cast<MsgFlags>(std::to_underlying(a) & std::to_underlying(b));
}

class MsgTag {
  std::uint64_t m_data{0};

 public:
  static constexpr std::uint8_t MAX_INLINE_WORDS = 6;
  static constexpr std::uint8_t MAX_OVERFLOW_WORDS = 57;
  static constexpr std::uint8_t MAX_TOTAL_WORDS = MAX_INLINE_WORDS + MAX_OVERFLOW_WORDS;
  static constexpr std::uint8_t MAX_EXTRA_CAPS = 4;

  constexpr MsgTag() noexcept = default;
  constexpr explicit MsgTag(const std::uint64_t raw) noexcept : m_data(raw) {}

  [[nodiscard]] static constexpr MsgTag make(const std::uint32_t label,
                                             const std::uint8_t length = 0,
                                             const std::uint8_t extra_caps = 0,
                                             const MsgFlags flags = MsgFlags::None,
                                             const Error status = Error::Success) noexcept {
    MsgTag tag{};
    tag.set_length(length <= MAX_TOTAL_WORDS ? length : MAX_TOTAL_WORDS);
    tag.set_extra_caps(extra_caps <= MAX_EXTRA_CAPS ? extra_caps : MAX_EXTRA_CAPS);
    tag.set_flags(flags);
    tag.set_status(static_cast<std::uint16_t>(std::to_underlying(status)));
    tag.set_label(label);
    return tag;
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, length, 0, 7)
  BF_RW(std::uint8_t, extra_caps, 7, 3)
  BF_RW(MsgFlags, flags, 10, 6)
  BF_RW(std::uint16_t, status, 16, 16)
  BF_RW(std::uint32_t, label, 32, 32)

  [[nodiscard]] constexpr Error error() const noexcept { return static_cast<Error>(get_status()); }
  [[nodiscard]] constexpr bool has_error() const noexcept {
    return (std::to_underlying(get_flags()) & std::to_underlying(MsgFlags::KernelError)) != 0 ||
           error() != Error::Success;
  }

  [[nodiscard]] constexpr bool is_notification() const noexcept {
    return (std::to_underlying(get_flags()) & std::to_underlying(MsgFlags::IsAsyncNotification)) !=
           0;
  }

  [[nodiscard]] constexpr bool operator==(const MsgTag&) const noexcept = default;
};
static_assert(sizeof(MsgTag) == sizeof(std::uint64_t));

struct alignas(64) Message {
  MsgTag tag{};
  std::uint64_t badge{0};
  std::array<std::uint64_t, MsgTag::MAX_INLINE_WORDS> words{};

  constexpr Message() noexcept = default;

  constexpr explicit Message(const std::uint32_t label, const std::uint64_t w0 = 0,
                             const std::uint64_t w1 = 0, const std::uint64_t w2 = 0,
                             const std::uint64_t w3 = 0, const std::uint64_t w4 = 0,
                             const std::uint64_t w5 = 0) noexcept
      : tag(MsgTag::make(label, MsgTag::MAX_INLINE_WORDS)), words{w0, w1, w2, w3, w4, w5} {}

  [[nodiscard]] static constexpr Message with_words(
      const std::uint32_t label, const std::uint8_t count, const std::uint64_t w0 = 0,
      const std::uint64_t w1 = 0, const std::uint64_t w2 = 0, const std::uint64_t w3 = 0,
      const std::uint64_t w4 = 0, const std::uint64_t w5 = 0) noexcept {
    Message msg{};
    msg.tag = MsgTag::make(label, count);
    msg.words = {w0, w1, w2, w3, w4, w5};
    return msg;
  }
};
static_assert(sizeof(Message) == 64);

struct CapTransfer {
  CapId src_cap{NULL_CAP};
  CapRights rights_mask{CapRights::All};
  std::uint8_t move{0};
  std::uint16_t reserved{0};
  std::uint64_t new_badge{0};
};
static_assert(sizeof(CapTransfer) == 16);

struct alignas(64) IpcBuffer {
  std::array<std::uint64_t, MsgTag::MAX_OVERFLOW_WORDS> overflow_words{};
  std::array<CapTransfer, MsgTag::MAX_EXTRA_CAPS> send_caps{};
  std::array<CapId, MsgTag::MAX_EXTRA_CAPS> recv_caps{};
  std::uint8_t received_cap_count{0};
};
static_assert(offsetof(IpcBuffer, overflow_words) == 0);
static_assert(offsetof(IpcBuffer, send_caps) == 456);
static_assert(offsetof(IpcBuffer, recv_caps) == 520);
static_assert(offsetof(IpcBuffer, received_cap_count) == 536);
static_assert(sizeof(IpcBuffer) == 576);

// Device, Thread, Process, and Memory Structures

struct DmaSegment {
  std::uint64_t phys_addr{0};
  std::uint64_t size_bytes{0};
};
static_assert(sizeof(DmaSegment) == 16);

struct DmaAllocation {
  void* virt_addr{nullptr};
  std::uint64_t first_phys_addr{0};
};

struct ForkResult {
  CapId child_process{NULL_CAP};  ///< Non-null in parent; `NULL_CAP` (0) in child.
  ProcessId child_pid{0};         ///< Child's PID in parent; 0 in child.

  [[nodiscard]] constexpr bool is_child() const noexcept { return child_process.is_null(); }
  [[nodiscard]] constexpr bool is_parent() const noexcept { return !child_process.is_null(); }
};

struct ThreadStats {
  std::uint64_t switch_count{0};
  std::uint64_t voluntary_switches{0};
  std::uint64_t involuntary_switches{0};
  std::uint64_t total_runtime_ns{0};
  std::uint64_t last_switch_in_ns{0};
  std::uint64_t creation_time_ns{0};
};
static_assert(sizeof(ThreadStats) == 48);

struct MemoryInfo {
  std::uint64_t total_bytes{0};
  std::uint64_t free_bytes{0};
  std::uint64_t quota_used{0};
  std::uint64_t quota_limit{0};
  std::uint64_t quota_available{0};
};
static_assert(sizeof(MemoryInfo) == 40);

/// @brief Labels delivered to a user-space fault handler endpoint (`ThreadConfigure`).
enum class FaultLabel : std::uint32_t {
  PageFault = 0xFA01u,      ///< words: [0]=fault_addr, [1]=access_flags, [2]=rip, [3]=tid
  UserException = 0xFA02u,  ///< words: [0]=vector, [1]=error_code, [2]=rip, [3]=tid
  CapFault = 0xFA03u,       ///< words: [0]=cap_id, [1]=error_code, [2]=rip, [3]=tid
};
}  // namespace user::sys