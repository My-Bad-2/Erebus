/// @file thread.hpp
/// @brief Threads of this process that the runtime sets up, and that can be waited for.
///
/// sys::thread::spawn() starts a thread with nothing but a stack. A Thread also has thread-local
/// storage and an IPC buffer of its own before its entry runs (see rt/runtime.hpp), so it can use
/// everything in sys/, and a notification it signals when its entry returns, which is what join()
/// waits on. join() then takes back what the runtime gave the thread: its storage and both
/// capabilities. Like std::jthread, a Thread that is destroyed while joinable joins first.
///
/// The thread has to end by returning from its entry. One that calls sys::thread::exit() itself
/// never signals, and joining it waits forever.
#pragma once

#include <string_view>

#include "sys/abi.hpp"
#include "sys/errors.hpp"

namespace user::rt {
namespace tls {
struct Tcb;
}  // namespace tls

class Thread {
 public:
  using Entry = void (*)(void* context) noexcept;

  constexpr Thread() noexcept = default;
  Thread(Thread&& other) noexcept;
  Thread& operator=(Thread&& other) noexcept;
  ~Thread() noexcept;

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  /// @brief Starts @p entry(@p context) on a new thread of this process. Names longer than the
  ///        kernel keeps are cut short.
  /// @return The thread; whatever the kernel says if it cannot have the storage, the notification
  ///         or the thread.
  [[nodiscard]] static sys::Result<Thread> spawn(std::string_view name, Entry entry,
                                                 void* context) noexcept;

  [[nodiscard]] bool joinable() const noexcept { return m_tcb != nullptr; }

  /// @brief The thread's capability, for sys::thread::stats() and the like. Null once joined.
  [[nodiscard]] sys::CapId cap() const noexcept;

  /// @brief Waits for the entry to return, and lets go of what the thread had.
  /// @return InvalidState for a thread that is not joinable, or the error that kept the runtime
  ///         from setting the thread up, in which case the entry never ran.
  sys::Result<void> join() noexcept;

 private:
  explicit Thread(tls::Tcb* tcb) noexcept : m_tcb(tcb) {}

  tls::Tcb* m_tcb{nullptr};
};
}  // namespace user::rt
