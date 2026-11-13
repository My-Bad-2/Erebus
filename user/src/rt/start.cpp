#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>

#include "rt/runtime.hpp"
#include "sys/syscalls.hpp"
#include "tls.hpp"

extern "C" {
using Hook = void (*)();

[[gnu::visibility("hidden")]] extern const Hook __preinit_array_start[];
[[gnu::visibility("hidden")]] extern const Hook __preinit_array_end[];
[[gnu::visibility("hidden")]] extern const Hook __init_array_start[];
[[gnu::visibility("hidden")]] extern const Hook __init_array_end[];
[[gnu::visibility("hidden")]] extern const Hook __fini_array_start[];
[[gnu::visibility("hidden")]] extern const Hook __fini_array_end[];
}

namespace user::rt {
namespace {
constexpr std::size_t MAX_EXIT_HANDLERS = 32;

constinit std::array<std::atomic<Hook>, MAX_EXIT_HANDLERS> g_exit_handlers{};
constinit std::atomic<std::size_t> g_exit_handler_count{0};
constinit std::atomic g_exiting{false};

void run_hooks(const Hook* const first, const Hook* const last) noexcept {
  for (const Hook* hook = first; hook != last; ++hook) {
    (*hook)();
  }
}
}  // namespace

void exit(const int status) noexcept {
  if (!g_exiting.exchange(true, std::memory_order_acq_rel)) {
    const std::size_t count =
        std::min(g_exit_handler_count.load(std::memory_order_acquire), MAX_EXIT_HANDLERS);
    for (std::size_t i = count; i-- > 0;) {
      if (const Hook handler = g_exit_handlers[i].load(std::memory_order_acquire)) {
        handler();
      }
    }

    for (const Hook* hook = __fini_array_end; hook != __fini_array_start;) {
      (*--hook)();
    }
  }

  (void)sys::proc::exit(status);
  std::unreachable();
}

void abort() noexcept {
  (void)sys::proc::exit(EXIT_ABORTED);
  std::unreachable();
}
}  // namespace user::rt

extern "C" {
int atexit(void (*const handler)()) noexcept {
  using user::rt::g_exit_handler_count;
  using user::rt::g_exit_handlers;
  using user::rt::MAX_EXIT_HANDLERS;

  const std::size_t slot = g_exit_handler_count.fetch_add(1, std::memory_order_acq_rel);
  if (slot >= MAX_EXIT_HANDLERS) [[unlikely]] {
    return -1;
  }

  g_exit_handlers[slot].store(handler, std::memory_order_release);
  return 0;
}

[[noreturn, gnu::section(".text._start")]] void _start() noexcept {
  using namespace user;

  const auto tcb = rt::tls::create();
  const auto ready =
      tcb.and_then([](rt::tls::Tcb* block) { return rt::tls::install(block); }).and_then([] {
        return sys::ipc::init_default_buffer().transform([](sys::IpcBuffer*) {});
      });
  if (!ready) [[unlikely]] {
    sys::debug::print("rt: cannot set up the first thread: {}", sys::to_string(ready.error()));
    (void)sys::proc::exit(rt::EXIT_NO_RUNTIME);
    std::unreachable();
  }

  rt::run_hooks(__preinit_array_start, __preinit_array_end);
  rt::run_hooks(__init_array_start, __init_array_end);
  rt::exit(user::main());
}
}
