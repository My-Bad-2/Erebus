// The user ABI headers (user/include/sys/) say again what the kernel says about system calls, for
// user code, which cannot include the kernel's headers. This holds the two side by side at compile
// time, value for value and offset for offset, so that changing one alone fails the build of the
// test kernel, which every build makes. There is nothing to run.
//
// Each list below names everything the user side has. A name added on one side only fails here,
// and so does one added on both with different values. One added on both and not listed here is
// the gap: whoever adds a call, an error or a field adds a line here too.
#include <cstddef>
#include <utility>

#include "core/cap.hpp"
#include "core/errors.hpp"
#include "core/ipc/driver_caps.hpp"
#include "core/ipc/message.hpp"
#include "core/ipc/object_cap.hpp"
#include "core/syscall/abi.hpp"
#include "memory/vmm/pagemap/flags.hpp"
#include "proc/process.hpp"
#include "sched/thread.hpp"
#include "sys/abi.hpp"
#include "sys/errors.hpp"

namespace {
namespace kipc = kernel::core::ipc;
namespace ksys = kernel::core::syscall;
namespace kvmm = kernel::memory::vmm;
namespace usys = user::sys;

template <typename A, typename B>
consteval bool same(const A a, const B b) noexcept {
  return std::to_underlying(a) == std::to_underlying(b);
}

// Numbers.
static_assert(ksys::FUNCTION_BITS == usys::FUNCTION_BITS);
static_assert(ksys::FAMILY_BITS == usys::FAMILY_BITS);
static_assert(ksys::FUNCTION_MASK == usys::FUNCTION_MASK);
static_assert(ksys::FAMILY_COUNT == usys::FAMILY_COUNT);

#define SAME_FAMILY(name) static_assert(same(ksys::Family::name, usys::Family::name), #name);
SAME_FAMILY(Ipc)
SAME_FAMILY(Cap)
SAME_FAMILY(Memory)
SAME_FAMILY(Device)
SAME_FAMILY(Process)
SAME_FAMILY(Thread)
SAME_FAMILY(Time)
SAME_FAMILY(Arch)
SAME_FAMILY(Debug)
#undef SAME_FAMILY

#define SAME_CALL(name) static_assert(same(ksys::Syscall::name, usys::Syscall::name), #name);
SAME_CALL(IpcCall)
SAME_CALL(IpcReplyRecv)
SAME_CALL(IpcSend)
SAME_CALL(IpcTrySend)
SAME_CALL(IpcRecv)
SAME_CALL(IpcTryRecv)
SAME_CALL(IpcReply)
SAME_CALL(IpcSaveReply)
SAME_CALL(IpcSignal)
SAME_CALL(IpcWait)
SAME_CALL(IpcPoll)
SAME_CALL(IpcSetBuffer)
SAME_CALL(CapCreate)
SAME_CALL(CapMint)
SAME_CALL(CapCopy)
SAME_CALL(CapMove)
SAME_CALL(CapDelete)
SAME_CALL(CapRevoke)
SAME_CALL(MemAllocate)
SAME_CALL(MemProtect)
SAME_CALL(MemUnmap)
SAME_CALL(MemMapObject)
SAME_CALL(MemDeriveObject)
SAME_CALL(MemResizeObject)
SAME_CALL(MemTranslateObject)
SAME_CALL(MemInfo)
SAME_CALL(DevIrqBind)
SAME_CALL(DevIrqAck)
SAME_CALL(DevMmioMap)
SAME_CALL(DevMmioDerive)
SAME_CALL(DevDmaAlloc)
SAME_CALL(DevDmaFree)
SAME_CALL(ProcCreate)
SAME_CALL(ProcFork)
SAME_CALL(ProcExit)
SAME_CALL(ProcWait)
SAME_CALL(ProcId)
SAME_CALL(ProcGrant)
SAME_CALL(ProcForkHere)
SAME_CALL(ThreadSpawn)
SAME_CALL(ThreadExit)
SAME_CALL(ThreadConfigure)
SAME_CALL(ThreadYield)
SAME_CALL(ThreadSetPriority)
SAME_CALL(ThreadId)
SAME_CALL(ThreadStats)
SAME_CALL(TimeNow)
SAME_CALL(TimeSleep)
SAME_CALL(ArchSetFsBase)
SAME_CALL(ArchGetFsBase)
SAME_CALL(ArchSetGsBase)
SAME_CALL(ArchGetGsBase)
SAME_CALL(ArchEnableAmx)
SAME_CALL(DebugLog)
#undef SAME_CALL

// Errors, and how they travel in RAX.
#define SAME_ERROR(name) static_assert(same(kernel::Error::name, usys::Error::name), #name);
SAME_ERROR(Success)
SAME_ERROR(HardwareError)
SAME_ERROR(NotImplemented)
SAME_ERROR(InvalidState)
SAME_ERROR(InvalidFlags)
SAME_ERROR(InvalidArgument)
SAME_ERROR(Timeout)
SAME_ERROR(NotFound)
SAME_ERROR(OutOfMemory)
SAME_ERROR(OutOfBounds)
SAME_ERROR(InvalidAlignment)
SAME_ERROR(AlreadyMapped)
SAME_ERROR(NotMapped)
SAME_ERROR(StackOverflow)
SAME_ERROR(PageFault)
SAME_ERROR(MemoryProtectionFault)
SAME_ERROR(QuotaExceeded)
SAME_ERROR(CasFailed)
SAME_ERROR(DeadlockDetected)
SAME_ERROR(WouldBlock)
SAME_ERROR(SecurityViolation)
SAME_ERROR(AccessDenied)
SAME_ERROR(IBTViolation)
SAME_ERROR(PrivilegeEscalationDetected)
SAME_ERROR(InstructionBoundaryMismatch)
SAME_ERROR(TargetTooSmallForTrampoline)
SAME_ERROR(ZydisRelocationFailed)
SAME_ERROR(ZydisConversionFailed)
SAME_ERROR(InvalidOpcode)
SAME_ERROR(TrampolineAllocationFailed)
SAME_ERROR(SiteOutOfTextBounds)
SAME_ERROR(ZydisDecodeFailed)
SAME_ERROR(ZydisEncodeFailed)
SAME_ERROR(UnexpectedSiteMnemonic)
SAME_ERROR(UnmappedGSI)
SAME_ERROR(ControllerLimitReached)
SAME_ERROR(OverrideLimitReached)
#undef SAME_ERROR

static_assert(ksys::encode_error(kernel::Error::QuotaExceeded) ==
              usys::encode_error(usys::Error::QuotaExceeded));
static_assert(same(ksys::decode_error(-0x3003), usys::decode_error(-0x3003)));

// Capabilities.
static_assert(kipc::CapId::INDEX_BITS == usys::CapId::INDEX_BITS);
static_assert(kipc::CapId::GEN_BITS == usys::CapId::GEN_BITS);
static_assert(kipc::CapId::make(0xABCDE, 0x123).raw() == usys::CapId::make(0xABCDE, 0x123).raw());

#define SAME_CAP_TYPE(name) static_assert(same(kipc::CapType::name, usys::CapType::name), #name);
SAME_CAP_TYPE(Null)
SAME_CAP_TYPE(Endpoint)
SAME_CAP_TYPE(Notification)
SAME_CAP_TYPE(Reply)
SAME_CAP_TYPE(VmObject)
SAME_CAP_TYPE(Mmio)
SAME_CAP_TYPE(IrqHandler)
SAME_CAP_TYPE(DmaPool)
SAME_CAP_TYPE(CSpace)
SAME_CAP_TYPE(Thread)
SAME_CAP_TYPE(Process)
#undef SAME_CAP_TYPE

#define SAME_RIGHT(name) static_assert(same(kipc::CapRights::name, usys::CapRights::name), #name);
SAME_RIGHT(None)
SAME_RIGHT(Read)
SAME_RIGHT(Write)
SAME_RIGHT(Grant)
SAME_RIGHT(GrantReply)
SAME_RIGHT(Mint)
SAME_RIGHT(Revoke)
SAME_RIGHT(Manage)
SAME_RIGHT(Execute)
SAME_RIGHT(All)
SAME_RIGHT(ReadWrite)
SAME_RIGHT(ClientCall)
SAME_RIGHT(ServerEndpoint)
SAME_RIGHT(DriverFull)
#undef SAME_RIGHT

#define SAME_CAP_FLAG(name) static_assert(same(kipc::CapFlags::name, usys::CapFlags::name), #name);
SAME_CAP_FLAG(None)
SAME_CAP_FLAG(OneShot)
SAME_CAP_FLAG(Badged)
SAME_CAP_FLAG(Pinned)
SAME_CAP_FLAG(Original)
#undef SAME_CAP_FLAG

// Memory. The kernel's access flags have more bits than user code may pass; these are the ones it
// may.
#define SAME_ACCESS(name) \
  static_assert(same(kvmm::AccessFlags::name, usys::AccessFlags::name), #name);
SAME_ACCESS(None)
SAME_ACCESS(Read)
SAME_ACCESS(Write)
SAME_ACCESS(Execute)
SAME_ACCESS(Stack)
SAME_ACCESS(Populate)
#undef SAME_ACCESS

static_assert(same(kvmm::PageSize::Size4K, usys::PageSize::Size4K));
static_assert(same(kvmm::PageSize::Size2M, usys::PageSize::Size2M));
static_assert(same(kvmm::PageSize::Size1G, usys::PageSize::Size1G));

#define SAME_CACHE(name) static_assert(same(kvmm::CacheMode::name, usys::CacheMode::name), #name);
SAME_CACHE(WriteBack)
SAME_CACHE(WriteThrough)
SAME_CACHE(UncacheableMinus)
SAME_CACHE(Uncacheable)
SAME_CACHE(WriteCombining)
SAME_CACHE(WriteProtected)
#undef SAME_CACHE

// Messages: the tag bit for bit, and the buffer byte for byte.
static_assert(kipc::MsgTag::MAX_INLINE_WORDS == usys::MsgTag::MAX_INLINE_WORDS);
static_assert(kipc::MsgTag::MAX_OVERFLOW_WORDS == usys::MsgTag::MAX_OVERFLOW_WORDS);
static_assert(kipc::MsgTag::MAX_TOTAL_WORDS == usys::MsgTag::MAX_TOTAL_WORDS);
static_assert(kipc::MsgTag::MAX_EXTRA_CAPS == usys::MsgTag::MAX_EXTRA_CAPS);

#define SAME_MSG_FLAG(name) static_assert(same(kipc::MsgFlags::name, usys::MsgFlags::name), #name);
SAME_MSG_FLAG(None)
SAME_MSG_FLAG(IsAsyncNotification)
SAME_MSG_FLAG(DonateSchedContext)
SAME_MSG_FLAG(KernelError)
SAME_MSG_FLAG(ReplyRequired)
#undef SAME_MSG_FLAG

static_assert(kipc::MsgTag::make(0xDEAD'BEEF, 63, 4, kipc::MsgFlags::KernelError,
                                 kernel::Error::WouldBlock)
                  .raw() == usys::MsgTag::make(0xDEAD'BEEF, 63, 4, usys::MsgFlags::KernelError,
                                               usys::Error::WouldBlock)
                                .raw());

static_assert(sizeof(kipc::Message) == sizeof(usys::Message));
static_assert(alignof(kipc::Message) == alignof(usys::Message));
static_assert(offsetof(kipc::Message, tag) == offsetof(usys::Message, tag));
static_assert(offsetof(kipc::Message, badge) == offsetof(usys::Message, badge));
static_assert(offsetof(kipc::Message, regs) == offsetof(usys::Message, words));

static_assert(sizeof(kipc::CapTransferItem) == sizeof(usys::CapTransfer));
static_assert(offsetof(kipc::CapTransferItem, src_cap) == offsetof(usys::CapTransfer, src_cap));
static_assert(offsetof(kipc::CapTransferItem, rights_mask) ==
              offsetof(usys::CapTransfer, rights_mask));
static_assert(offsetof(kipc::CapTransferItem, move_cap) == offsetof(usys::CapTransfer, move));
static_assert(offsetof(kipc::CapTransferItem, new_badge) == offsetof(usys::CapTransfer, new_badge));

static_assert(sizeof(kipc::IpcBuffer) == sizeof(usys::IpcBuffer));
static_assert(alignof(kipc::IpcBuffer) == alignof(usys::IpcBuffer));
static_assert(offsetof(kipc::IpcBuffer, overflow_words) ==
              offsetof(usys::IpcBuffer, overflow_words));
static_assert(offsetof(kipc::IpcBuffer, send_caps) == offsetof(usys::IpcBuffer, send_caps));
static_assert(offsetof(kipc::IpcBuffer, recv_caps) == offsetof(usys::IpcBuffer, recv_caps));
static_assert(offsetof(kipc::IpcBuffer, received_cap_count) ==
              offsetof(usys::IpcBuffer, received_cap_count));

static_assert(same(kipc::FaultMsgLabel::PageFault, usys::FaultLabel::PageFault));
static_assert(same(kipc::FaultMsgLabel::UserException, usys::FaultLabel::UserException));
static_assert(same(kipc::FaultMsgLabel::CapFault, usys::FaultLabel::CapFault));

// What calls write to user memory.
static_assert(sizeof(ksys::MemoryInfo) == sizeof(usys::MemoryInfo));
static_assert(offsetof(ksys::MemoryInfo, total_bytes) == offsetof(usys::MemoryInfo, total_bytes));
static_assert(offsetof(ksys::MemoryInfo, free_bytes) == offsetof(usys::MemoryInfo, free_bytes));
static_assert(offsetof(ksys::MemoryInfo, quota_used) == offsetof(usys::MemoryInfo, quota_used));
static_assert(offsetof(ksys::MemoryInfo, quota_limit) == offsetof(usys::MemoryInfo, quota_limit));
static_assert(offsetof(ksys::MemoryInfo, quota_available) ==
              offsetof(usys::MemoryInfo, quota_available));

static_assert(sizeof(kernel::sched::ThreadStats) == sizeof(usys::ThreadStats));
static_assert(offsetof(kernel::sched::ThreadStats, switch_count) ==
              offsetof(usys::ThreadStats, switch_count));
static_assert(offsetof(kernel::sched::ThreadStats, voluntary_switches) ==
              offsetof(usys::ThreadStats, voluntary_switches));
static_assert(offsetof(kernel::sched::ThreadStats, involuntary_switches) ==
              offsetof(usys::ThreadStats, involuntary_switches));
static_assert(offsetof(kernel::sched::ThreadStats, total_runtime_ns) ==
              offsetof(usys::ThreadStats, total_runtime_ns));
static_assert(offsetof(kernel::sched::ThreadStats, last_switch_in_ns) ==
              offsetof(usys::ThreadStats, last_switch_in_ns));
static_assert(offsetof(kernel::sched::ThreadStats, creation_time_ns) ==
              offsetof(usys::ThreadStats, creation_time_ns));

static_assert(sizeof(kipc::DmaSegment) == sizeof(usys::DmaSegment));
static_assert(offsetof(kipc::DmaSegment, phys_addr) == offsetof(usys::DmaSegment, phys_addr));
static_assert(offsetof(kipc::DmaSegment, size_bytes) == offsetof(usys::DmaSegment, size_bytes));

// Identifiers.
static_assert(sizeof(kernel::proc::ProcessId) == sizeof(usys::ProcessId));
static_assert(kernel::proc::ANY_CHILD == usys::ANY_CHILD);
static_assert(sizeof(kernel::sched::ThreadId) == sizeof(usys::ThreadId));
}  // namespace
