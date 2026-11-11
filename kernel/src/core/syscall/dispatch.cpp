#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "core/syscall/abi.hpp"
#include "core/syscall/syscall.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "handlers.hpp"
#include "utils/assert.hpp"

namespace kernel::core::syscall {
namespace handlers {
std::int64_t not_implemented(Frame&) noexcept { return encode_error(Error::NotImplemented); }
}  // namespace handlers

namespace {
struct Route {
  Syscall call;
  Handler handler;
};

template <auto Fn>
consteval Handler as_handler() {
  if constexpr (std::is_convertible_v<decltype(Fn), Handler>) {
    return Fn;
  } else {
    return [](Frame& frame) noexcept { return Fn(frame); };
  }
}

template <Syscall Call, auto Fn>
consteval Route route() {
  return {.call = Call, .handler = as_handler<Fn>()};
}

// Routes syscall to its handler.
constexpr Route ROUTES[] = {
    route<Syscall::IpcCall, &handlers::ipc_call>(),
    route<Syscall::IpcReplyRecv, &handlers::ipc_reply_recv>(),
    route<Syscall::IpcSend, &handlers::ipc_send>(),
    route<Syscall::IpcTrySend, &handlers::ipc_try_send>(),
    route<Syscall::IpcRecv, &handlers::ipc_recv>(),
    route<Syscall::IpcTryRecv, &handlers::ipc_try_recv>(),
    route<Syscall::IpcReply, &handlers::ipc_reply>(),
    route<Syscall::IpcSaveReply, &handlers::ipc_save_reply>(),
    route<Syscall::IpcSignal, &handlers::ipc_signal>(),
    route<Syscall::IpcWait, &handlers::ipc_wait>(),
    route<Syscall::IpcPoll, &handlers::ipc_poll>(),
    route<Syscall::IpcSetBuffer, &handlers::ipc_set_buffer>(),

    route<Syscall::CapCreate, &handlers::cap_create>(),
    route<Syscall::CapMint, &handlers::cap_mint>(),
    route<Syscall::CapCopy, &handlers::cap_copy>(),
    route<Syscall::CapMove, &handlers::cap_move>(),
    route<Syscall::CapDelete, &handlers::cap_delete>(),
    route<Syscall::CapRevoke, &handlers::cap_revoke>(),

    route<Syscall::MemAllocate, &handlers::mem_allocate>(),
    route<Syscall::MemProtect, &handlers::mem_protect>(),
    route<Syscall::MemUnmap, &handlers::mem_unmap>(),
    route<Syscall::MemMapObject, &handlers::mem_map_object>(),
    route<Syscall::MemDeriveObject, &handlers::mem_derive_object>(),
    route<Syscall::MemResizeObject, &handlers::mem_resize_object>(),
    route<Syscall::MemTranslateObject, &handlers::mem_translate_object>(),
    route<Syscall::MemInfo, &handlers::mem_info>(),

    route<Syscall::DevIrqBind, &handlers::dev_irq_bind>(),
    route<Syscall::DevIrqAck, &handlers::dev_irq_ack>(),
    route<Syscall::DevMmioMap, &handlers::dev_mmio_map>(),
    route<Syscall::DevMmioDerive, &handlers::dev_mmio_derive>(),
    route<Syscall::DevDmaAlloc, &handlers::dev_dma_alloc>(),
    route<Syscall::DevDmaFree, &handlers::dev_dma_free>(),

    route<Syscall::ProcCreate, &handlers::proc_create>(),
    route<Syscall::ProcFork, &handlers::proc_fork>(),
    route<Syscall::ProcExit, &handlers::proc_exit>(),
    route<Syscall::ProcWait, &handlers::proc_wait>(),
    route<Syscall::ProcId, &handlers::proc_id>(),
    route<Syscall::ProcGrant, &handlers::proc_grant>(),
    route<Syscall::ProcForkHere, &handlers::proc_fork_here>(),

    route<Syscall::ThreadSpawn, &handlers::thread_spawn>(),
    route<Syscall::ThreadExit, &handlers::thread_exit>(),
    route<Syscall::ThreadConfigure, &handlers::thread_configure>(),
    route<Syscall::ThreadYield, &handlers::thread_yield>(),
    route<Syscall::ThreadSetPriority, &handlers::not_implemented>(),
    route<Syscall::ThreadId, &handlers::thread_id>(),
    route<Syscall::ThreadStats, &handlers::thread_stats>(),

    route<Syscall::TimeNow, &handlers::time_now>(),
    route<Syscall::TimeSleep, &handlers::time_sleep>(),

    route<Syscall::ArchSetFsBase, &handlers::arch_set_fs_base>(),
    route<Syscall::ArchGetFsBase, &handlers::arch_get_fs_base>(),
    route<Syscall::ArchSetGsBase, &handlers::arch_set_gs_base>(),
    route<Syscall::ArchGetGsBase, &handlers::arch_get_gs_base>(),
    route<Syscall::ArchEnableAmx, &handlers::arch_enable_amx>(),

    route<Syscall::DebugLog, &handlers::debug_log>(),
};

struct Span {
  std::uint16_t first;
  std::uint16_t count;
};

consteval bool families_are_known() {
  return std::ranges::all_of(ROUTES, [](const Route& route) {
    return std::to_underlying(family_of(route.call)) < FAMILY_COUNT && route.handler != nullptr;
  });
}

static_assert(families_are_known(), "a route without a handler, or in a family past FAMILY_COUNT");

consteval std::array<Span, FAMILY_COUNT> build_spans() {
  std::array<std::uint16_t, FAMILY_COUNT> counts{};
  for (const Route& route : ROUTES) {
    std::uint16_t& count = counts[std::to_underlying(family_of(route.call))];
    count = std::max<std::uint16_t>(count, function_of(route.call) + 1);
  }

  std::array<Span, FAMILY_COUNT> spans{};
  std::uint16_t next = 0;
  for (std::size_t family = 0; family < FAMILY_COUNT; ++family) {
    spans[family] = {.first = next, .count = counts[family]};
    next += counts[family];
  }

  return spans;
}

constexpr std::array<Span, FAMILY_COUNT> SPANS = build_spans();
constexpr std::size_t HANDLER_COUNT = SPANS.back().first + SPANS.back().count;

[[nodiscard]] consteval std::size_t index_of(const Syscall call) {
  return SPANS[std::to_underlying(family_of(call))].first + function_of(call);
}

consteval bool routes_are_unique() {
  for (std::size_t i = 0; i < std::size(ROUTES); ++i) {
    for (std::size_t j = i + 1; j < std::size(ROUTES); ++j) {
      if (ROUTES[i].call == ROUTES[j].call) {
        return false;
      }
    }
  }

  return true;
}

static_assert(routes_are_unique(), "two routes for one system call");

consteval std::array<Handler, HANDLER_COUNT> build_handlers() {
  std::array<Handler, HANDLER_COUNT> table{};
  table.fill(&handlers::not_implemented);
  for (const Route& route : ROUTES) {
    table[index_of(route.call)] = route.handler;
  }

  return table;
}

constexpr std::array<Handler, HANDLER_COUNT> HANDLERS = build_handlers();

// @p index if it is below @p size, and 0 if not, worked out with arithmetic rather than a branch.
// The bounds checks below branch, and a CPU that guesses one wrong would otherwise load a handler
// from wherever a number from user code points, and jump to it, if only speculatively.
[[nodiscard, gnu::always_inline]] std::size_t clamp_index(std::size_t index,
                                                          std::size_t size) noexcept {
  std::size_t mask;
  asm("cmpq %[size], %[index]\n\t"
      "sbbq %[mask], %[mask]"
      : [mask] "=r"(mask)
      : [index] "r"(index), [size] "re"(size)
      : "cc");
  return index & mask;
}

[[nodiscard]] Handler find(const std::uint64_t number) noexcept {
  // Bits above the family make it one no table has, so they need no check of their own.
  const std::uint64_t family = number >> FUNCTION_BITS;
  const std::uint64_t function = number & FUNCTION_MASK;
  if (family >= FAMILY_COUNT) [[unlikely]] {
    return &handlers::not_implemented;
  }

  const Span span = SPANS[clamp_index(family, FAMILY_COUNT)];
  if (function >= span.count) [[unlikely]] {
    return &handlers::not_implemented;
  }

  return HANDLERS[span.first + clamp_index(function, span.count)];
}
}  // namespace

void handle(hw::interrupts::RegisterContext& regs, const UserReturn& from) noexcept {
  Frame frame{regs, from};
  const Handler handler = find(frame.number());

  hw::irq::enable();
  KASSERT(hw::percpu::can_sleep());

  frame.set_result(handler(frame));

  KASSERT(hw::percpu::can_sleep());
  hw::irq::disable();
}
}  // namespace kernel::core::syscall
