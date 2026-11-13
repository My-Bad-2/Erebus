// The Time, Arch and Debug families: small calls about the calling thread and the machine.
#include <algorithm>
#include <array>
#include <chrono.hpp>
#include <limits>
#include <string_view>

#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/user.hpp"
#include "handlers.hpp"
#include "proc/process.hpp"
#include "sched/sleep.hpp"
#include "sched/thread.hpp"
#include "utils/logger.hpp"

namespace kernel::core::syscall::handlers {
namespace {
constexpr std::size_t MAX_LOG_BYTES = 512;
constexpr std::uint64_t SLEEP_ABSOLUTE = 1u << 0;

[[nodiscard]] klib::Duration nanoseconds(const std::uint64_t raw) noexcept {
  constexpr auto LONGEST = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return klib::Duration::nanoseconds(static_cast<std::int64_t>(std::min(raw, LONGEST)));
}

// A thread's FS and GS bases are in two places: its saved state, and the CPU while it runs, where
// user code may change them behind the kernel's back (CR4.FSGSBASE). A switch reads them back into
// the saved state, so both are changed without one coming in between.
class [[nodiscard]] NoPreemption {
 public:
  NoPreemption() noexcept { hw::percpu::preempt_disable(); }
  ~NoPreemption() noexcept { hw::percpu::preempt_enable(); }

  NoPreemption(const NoPreemption&) = delete;
  NoPreemption& operator=(const NoPreemption&) = delete;
};

// 0, or an address user code could use. The CPU would raise #GP for one that is not canonical,
// and in the kernel, which is where the base is loaded.
[[nodiscard]] bool is_valid_base(const std::uint64_t base) noexcept {
  return base == 0 || hw::user::is_user_address(memory::VirtualAddress{base});
}
}  // namespace

std::int64_t time_now(Frame&) noexcept {
  return static_cast<std::int64_t>(drivers::SystemClock::now().raw_ns());
}

std::int64_t time_sleep(Frame& frame) noexcept {
  const std::uint64_t flags = frame.arg<2>();
  if ((flags & ~SLEEP_ABSOLUTE) != 0) [[unlikely]] {
    return encode_error(Error::InvalidFlags);
  }

  const klib::Duration slack = nanoseconds(frame.arg<1>());
  const bool finished = (flags & SLEEP_ABSOLUTE) != 0
                            ? sched::sleep_until(klib::TimePoint{frame.arg<0>()}, slack)
                            : sched::sleep_for(nanoseconds(frame.arg<0>()), slack);
  return finished ? 0 : encode_error(Error::WouldBlock);
}

std::int64_t arch_set_fs_base(Frame& frame) noexcept {
  const std::uint64_t base = frame.arg<0>();
  if (!is_valid_base(base)) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  const NoPreemption no_preemption;
  sched::current()->arch_state().fs_base = base;
  hw::write::fs_base(base);
  return 0;
}

std::int64_t arch_get_fs_base(Frame&) noexcept {
  const NoPreemption no_preemption;
  const std::uint64_t base =
      hw::user::can_change_bases() ? hw::read::fs_base() : sched::current()->arch_state().fs_base;
  return static_cast<std::int64_t>(base);
}

std::int64_t arch_set_gs_base(Frame& frame) noexcept {
  const std::uint64_t base = frame.arg<0>();
  if (!is_valid_base(base)) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  const NoPreemption no_preemption;
  sched::current()->arch_state().user_gs_base = base;
  hw::user::write_gs_base(base);
  hw::percpu::self()->user_gs_base = base;
  return 0;
}

std::int64_t arch_get_gs_base(Frame&) noexcept {
  const NoPreemption no_preemption;
  const std::uint64_t base = hw::user::can_change_bases()
                                 ? hw::user::read_gs_base()
                                 : sched::current()->arch_state().user_gs_base;
  return static_cast<std::int64_t>(base);
}

std::int64_t arch_enable_amx(Frame&) noexcept {
  return to_rax(sched::current()->request_amx_context());
}

std::int64_t debug_log(Frame& frame) noexcept {
  std::array<char, MAX_LOG_BYTES> buffer{};
  const auto copied = copy_string_from_user(frame.address<0>(), frame.arg<1>(), buffer);
  if (!copied) [[unlikely]] {
    return encode_error(copied.error());
  }

  std::string_view line = *copied;
  if (line.ends_with('\n')) {
    line.remove_suffix(1);
  }

  const std::span<char> text = std::span{buffer}.first(line.size());
  std::ranges::replace_if(
      text, [](const char c) { return c != '\t' && (c < 0x20 || c > 0x7E); }, '?');

  // The line is the process's, so it says so where a kernel line says which file and line it is
  // from: [name:pid].
  const proc::Process* const process = proc::current_process();
  std::array<char, proc::Process::MAX_NAME_LEN + 16> origin{};
  const std::size_t origin_length = klib::kprint(origin.data(), origin.size(), "[{}:{}]",
                                                 process != nullptr ? process->name() : "?",
                                                 process != nullptr ? process->pid() : 0u);

  utils::logger::info_from(
      std::string_view{origin.data(), std::min(origin_length, origin.size() - 1)}, "{}",
      std::string_view{text.data(), text.size()});
  return 0;
}
}  // namespace kernel::core::syscall::handlers
