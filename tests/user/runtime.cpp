// The user runtime (rt/runtime.hpp, rt/thread.hpp): what a program can count on before main(),
// and its threads.
#include "rt/runtime.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "ktest.hpp"
#include "rt/thread.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
// One of each kind of thread-local variable: from .tdata, and from .tbss.
constinit thread_local std::uint64_t t_initialised = 0x5EED'5EED;
constinit thread_local std::uint64_t t_zeroed = 0;

// Made by a constructor, which only the runtime runs: the value is not known to the compiler.
struct Constructed {
  std::uint64_t made_at;
  Constructed() noexcept : made_at(sys::time::now()) {}
};
Constructed g_constructed;

std::uint64_t g_constructor_ran_at = 0;
[[gnu::constructor]] void note_constructor() { g_constructor_ran_at = sys::time::now(); }

[[nodiscard]] void* fs_word_zero() noexcept {
  void* self = nullptr;
  asm volatile("movq %%fs:0, %0" : "=r"(self));
  return self;
}

struct TlsView {
  std::uint64_t initialised;
  std::uint64_t zeroed;
  void* fs_self;
  void* fs_base;
  sys::IpcBuffer* buffer;
  std::uintptr_t initialised_at;
};

[[nodiscard]] TlsView look_at_tls() noexcept {
  const auto base = sys::arch::get_fs_base();
  return TlsView{
      .initialised = t_initialised,
      .zeroed = t_zeroed,
      .fs_self = fs_word_zero(),
      .fs_base = base ? *base : nullptr,
      .buffer = sys::ipc::default_buffer(),
      .initialised_at = reinterpret_cast<std::uintptr_t>(&t_initialised),
  };
}

void look_from_a_thread(void* const context) noexcept {
  *static_cast<TlsView*>(context) = look_at_tls();
  t_initialised = 0;  // its own copy
}
}  // namespace

KTEST(Runtime, ran_the_constructors_before_main) {
  KEXPECT_NE(g_constructed.made_at, 0u);
  KEXPECT_NE(g_constructor_ran_at, 0u);
  KEXPECT_LE(g_constructed.made_at, sys::time::now());
}

KTEST(Runtime, fs_points_at_a_block_that_points_at_itself) {
  const TlsView view = look_at_tls();
  KREQUIRE(view.fs_base != nullptr);
  KEXPECT_EQ(view.fs_self, view.fs_base);

  // Variant II: the thread's copy of the template sits right below the block FS points at.
  const auto tp = reinterpret_cast<std::uintptr_t>(view.fs_base);
  KEXPECT_LT(view.initialised_at, tp);
  KEXPECT_GT(view.initialised_at + 4096, tp);
}

KTEST(Runtime, each_thread_gets_a_fresh_copy_of_thread_local_storage) {
  t_initialised = 1;
  t_zeroed = 2;

  TlsView theirs{};
  auto thread = rt::Thread::spawn("tls", &look_from_a_thread, &theirs);
  KREQUIRE_OK(thread);
  KEXPECT_OK(thread->join());

  const TlsView mine = look_at_tls();
  KEXPECT_EQ(theirs.initialised, 0x5EED'5EEDu);
  KEXPECT_EQ(theirs.zeroed, 0u);
  KEXPECT_EQ(theirs.fs_self, theirs.fs_base);
  KEXPECT_NE(theirs.fs_base, mine.fs_base);
  KEXPECT_NE(theirs.initialised_at, mine.initialised_at);

  // The thread wrote to its own copy, not this one.
  KEXPECT_EQ(t_initialised, 1u);
  KEXPECT_EQ(t_zeroed, 2u);
  t_initialised = 0x5EED'5EED;
  t_zeroed = 0;
}

KTEST(Runtime, each_thread_has_an_ipc_buffer_of_its_own) {
  TlsView theirs{};
  auto thread = rt::Thread::spawn("ipc buffer", &look_from_a_thread, &theirs);
  KREQUIRE_OK(thread);
  KEXPECT_OK(thread->join());

  const TlsView mine = look_at_tls();
  KEXPECT(mine.buffer != nullptr);
  KEXPECT(theirs.buffer != nullptr);
  KEXPECT(mine.buffer != theirs.buffer);
}

KTEST(Runtime, join_waits_for_the_entry_to_return) {
  constexpr std::uint32_t THREADS = 8;
  static std::atomic<std::uint32_t> s_finished{0};
  s_finished.store(0);

  std::array<rt::Thread, THREADS> threads{};
  for (rt::Thread& thread : threads) {
    auto spawned = rt::Thread::spawn(
        "join",
        [](void*) noexcept {
          (void)sys::time::sleep_for(1'000'000);
          s_finished.fetch_add(1);
        },
        nullptr);
    KREQUIRE_OK(spawned);
    KEXPECT_NE(spawned->cap(), sys::NULL_CAP);
    thread = std::move(*spawned);
  }

  for (rt::Thread& thread : threads) {
    KEXPECT_OK(thread.join());
    KEXPECT(!thread.joinable());
    KEXPECT_ERROR(thread.join(), sys::Error::InvalidState);
  }

  KEXPECT_EQ(s_finished.load(), THREADS);
}

KTEST(Runtime, a_thread_left_joinable_is_joined_when_it_goes) {
  static std::atomic<bool> s_done{false};
  s_done.store(false);

  {
    auto thread = rt::Thread::spawn(
        "scoped",
        [](void*) noexcept {
          (void)sys::time::sleep_for(2'000'000);
          s_done.store(true);
        },
        nullptr);
    KREQUIRE_OK(thread);
  }

  KEXPECT(s_done.load());
}

// Handlers run last first: the second one notes that it ran, and the first one ends the process
// with a status that says whether the second came before it. Without them the status is 1.
KTEST(Runtime, exit_runs_the_handlers_last_first) {
  static bool s_second_ran = false;

  const auto status = in_child("atexit", []() -> int {
    s_second_ran = false;
    KEXPECT_EQ(atexit([] { (void)sys::proc::exit(s_second_ran ? 42 : 7); }), 0);
    KEXPECT_EQ(atexit([] { s_second_ran = true; }), 0);
    rt::exit(1);
  });

  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, 42);
}

KTEST(Runtime, abort_ends_the_process_with_a_status_of_its_own) {
  const auto status = in_child("abort", []() -> int { rt::abort(); });
  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, rt::EXIT_ABORTED);
}

KTEST(Runtime, main_returning_ends_the_process_with_its_result) {
  // What the runtime does with main()'s result is rt::exit(), here for a child that has no
  // handlers to run.
  const auto status = in_child("exit", []() -> int { rt::exit(5); });
  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, 5);
}
}  // namespace user::testing
