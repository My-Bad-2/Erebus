/// @file ipi.hpp
/// @brief Cross-CPU function calls.
///
/// Any code can ask other CPUs to run a function and, optionally, wait until they have. This is
/// what TLB shootdowns, cache maintenance, timer reprogramming and friends are built on.
///
/// How it works:
///
///  - Every CPU has an inbox: a bounded, lock-free queue of pointers to call requests. Posting is
///    lock-free for any number of senders; only the owner takes requests out.
///  - A call to many CPUs is one request, posted to every target's inbox. Each target counts
///    the request down when it is done, so the sender waits on a single counter.
///  - Posting does not always cost an interrupt. Each inbox has a "kicked" flag that is set by
///    whoever sends the IPI and cleared by the target before it empties the inbox; senders that
///    find it set know an IPI is already on the way and skip theirs.
///  - Interrupts go out as cheaply as the hardware allows: with the "all but self" shorthand when
///    every CPU is targeted, as one x2APIC logical IPI per cluster of up to 16 CPUs otherwise,
///    and as plain physical IPIs in xAPIC mode.
///  - A CPU that waits for anything here (a reply, a full inbox, a free request slot) keeps
///    emptying its own inbox meanwhile. Two CPUs calling each other at the same moment, even with
///    interrupts disabled, therefore both make progress.
///
/// Called functions run on the target in interrupt context with interrupts disabled. They may
/// take spinlocks and make calls of their own, but must not sleep, and should be short: the
/// target is not doing anything else meanwhile.

#pragma once

#include <cstdint>
#include <expected>

#include "core/errors.hpp"

namespace kernel::hw::ipi {
/// @brief Function run on a target CPU.
using Function = void (*)(void* arg);

/// @brief Picks targets for call_many(). @p cpu is a dense CPU ID.
using Selector = bool (*)(std::uint32_t cpu, const void* ctx);

/// @brief Whether the caller waits for the targets to finish.
enum class Wait : bool {
  /// Return once the call is posted. Whatever @p arg points to must stay valid until every
  /// target has run the function, and the caller has no way of knowing when that is, so pass
  /// static data (or nothing).
  No = false,
  /// Return once every target has run the function. @p arg may point at the caller's stack.
  Yes = true,
};

/// @brief Runs `fn(arg)` on CPU @p cpu.
///
/// Calling the current CPU runs the function right away, with interrupts disabled like a remote
/// call would.
/// @return InvalidArgument for a CPU that does not exist, InvalidState for one that is not online.
std::expected<void, Error> call(std::uint32_t cpu, Function fn, void* arg,
                                Wait wait = Wait::Yes) noexcept;

/// @brief Runs `fn(arg)` on every other online CPU for which `select(cpu, ctx)` is true.
///
/// Everything the caller wrote before the call is visible to the function. A CPU that comes
/// online during the call is either included or comes up after the caller's writes and sees
/// them anyway.
/// @param select Null selects every other online CPU.
/// @return How many CPUs the call was posted to.
std::uint32_t call_many(Selector select, const void* ctx, Function fn, void* arg,
                        Wait wait = Wait::Yes) noexcept;

/// @brief Runs `fn(arg)` on every other online CPU.
/// @return How many CPUs the call was posted to.
inline std::uint32_t call_others(const Function fn, void* arg,
                                 const Wait wait = Wait::Yes) noexcept {
  return call_many(nullptr, nullptr, fn, arg, wait);
}

/// @brief Runs `fn(arg)` on every online CPU, the caller included.
///
/// The caller runs its share while the others run theirs.
void call_all(Function fn, void* arg, Wait wait = Wait::Yes) noexcept;

/// @brief Interrupts CPU @p cpu so that it reconsiders what to run.
///
/// The interrupt does nothing by itself; it exists to wake a CPU out of its idle loop.
void send_reschedule(std::uint32_t cpu) noexcept;

/// @brief Runs whatever is waiting in this CPU's inbox.
///
/// For code that spins with interrupts disabled for a long time and wants to stay responsive.
/// The waits inside this module already do it.
void poll() noexcept;

/// @brief Allocates the inboxes and installs the IPI handlers. Called once, by the BSP.
void initialize() noexcept;
}  // namespace kernel::hw::ipi
