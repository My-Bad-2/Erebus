#include "rt/thread.hpp"

#include <cstdint>
#include <utility>

#include "sys/syscalls.hpp"
#include "tls.hpp"

namespace user::rt {
namespace {
[[noreturn]] void thread_main(const std::uint64_t raw) noexcept {
  auto* const tcb = reinterpret_cast<tls::Tcb*>(raw);

  const auto ready = tls::install(tcb).and_then(
      [] { return sys::ipc::init_default_buffer().transform([](sys::IpcBuffer*) {}); });
  if (ready) [[likely]] {
    tcb->entry(tcb->context);
  } else {
    tcb->failure = ready.error();
  }

  const sys::CapId done = tcb->done;
  (void)sys::ipc::signal(done);
  (void)sys::thread::exit();
  std::unreachable();
}
}  // namespace

Thread::Thread(Thread&& other) noexcept : m_tcb(std::exchange(other.m_tcb, nullptr)) {}

Thread& Thread::operator=(Thread&& other) noexcept {
  if (this != &other) {
    if (joinable()) {
      (void)join();
    }

    m_tcb = std::exchange(other.m_tcb, nullptr);
  }

  return *this;
}

Thread::~Thread() noexcept {
  if (joinable()) {
    (void)join();
  }
}

sys::Result<Thread> Thread::spawn(const std::string_view name, const Entry entry,
                                  void* const context) noexcept {
  const auto tcb = tls::create();
  if (!tcb) [[unlikely]] {
    return std::unexpected(tcb.error());
  }

  const auto done = sys::cap::create_notification();
  if (!done) [[unlikely]] {
    tls::destroy(*tcb);
    return std::unexpected(done.error());
  }

  (*tcb)->entry = entry;
  (*tcb)->context = context;
  (*tcb)->done = *done;

  const auto thread = sys::thread::spawn(name, &thread_main, reinterpret_cast<std::uint64_t>(*tcb));
  if (!thread) [[unlikely]] {
    (void)sys::cap::remove(sys::NULL_CAP, *done);
    tls::destroy(*tcb);
    return std::unexpected(thread.error());
  }

  (*tcb)->thread = *thread;
  return Thread{*tcb};
}

sys::CapId Thread::cap() const noexcept { return m_tcb != nullptr ? m_tcb->thread : sys::NULL_CAP; }

sys::Result<void> Thread::join() noexcept {
  if (m_tcb == nullptr) [[unlikely]] {
    return std::unexpected(sys::Error::InvalidState);
  }

  const auto signalled = sys::ipc::wait(m_tcb->done);
  if (!signalled) [[unlikely]] {
    return std::unexpected(signalled.error());
  }

  tls::Tcb* const tcb = std::exchange(m_tcb, nullptr);
  const sys::Error failure = tcb->failure;

  (void)sys::cap::remove(sys::NULL_CAP, tcb->done);
  (void)sys::cap::remove(sys::NULL_CAP, tcb->thread);
  tls::destroy(tcb);

  if (failure != sys::Error::Success) [[unlikely]] {
    return std::unexpected(failure);
  }

  return {};
}
}  // namespace user::rt
