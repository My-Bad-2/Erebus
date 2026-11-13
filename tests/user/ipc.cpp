// The Ipc family from user code: notifications, and messages between threads and processes,
// short ones in registers and long ones through each thread's IPC buffer.
#include <array>
#include <atomic>
#include <cstdint>

#include "ktest.hpp"
#include "rt/thread.hpp"
#include "support.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
using sys::CapId;
using sys::Error;
using sys::Message;
using sys::NULL_CAP;

constexpr std::uint32_t LABEL = 0x1AB;

// What a server thread received, and what it got wrong, for the test to look at once it joined.
struct Server {
  CapId endpoint;
  Message received{};
  std::array<std::uint64_t, sys::MsgTag::MAX_TOTAL_WORDS> words{};
  CapId received_cap{NULL_CAP};
  std::uint8_t received_caps{0};
  Error error{Error::Success};
  int rounds{1};
};

void receive_one(void* const context) noexcept {
  auto& server = *static_cast<Server*>(context);
  const auto message = sys::ipc::recv(server.endpoint);
  if (!message) {
    server.error = message.error();
    return;
  }

  server.received = *message;
  const std::size_t length = message->tag.get_length();
  const sys::IpcBuffer* const buffer = sys::ipc::default_buffer();
  for (std::size_t i = 0; i < length; ++i) {
    server.words[i] = i < sys::MsgTag::MAX_INLINE_WORDS
                          ? message->words[i]
                          : buffer->overflow_words[i - sys::MsgTag::MAX_INLINE_WORDS];
  }

  server.received_caps = buffer->received_cap_count;
  server.received_cap = buffer->recv_caps[0];
}

// Answers @p rounds calls, each with every word plus one, and the badge it came with in word 5.
void answer_calls(void* const context) noexcept {
  auto& server = *static_cast<Server*>(context);
  auto request = sys::ipc::recv(server.endpoint);
  for (int round = 0; request && round < server.rounds; ++round) {
    Message answer{LABEL + 1};
    for (std::size_t i = 0; i < 5; ++i) {
      answer.words[i] = request->words[i] + 1;
    }
    answer.words[5] = request->badge;

    if (round + 1 == server.rounds) {
      if (const auto replied = sys::ipc::reply(answer); !replied) {
        server.error = replied.error();
      }
      return;
    }

    request = sys::ipc::reply_recv(server.endpoint, answer);
  }

  if (!request) {
    server.error = request.error();
  }
}
}  // namespace

KTEST(Notification, poll_takes_every_bit_at_once_and_leaves_none) {
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(notification);
  const auto badged = sys::cap::mint(NULL_CAP, *notification, NULL_CAP, sys::CapRights::Write, 0x8);
  KREQUIRE_OK(badged);

  KEXPECT_EQ(sys::ipc::poll(*notification).value_or(1), 0u);
  KEXPECT_OK(sys::ipc::signal(*notification));
  KEXPECT_OK(sys::ipc::signal(*badged));
  KEXPECT_OK(sys::ipc::signal(*badged));
  KEXPECT_EQ(sys::ipc::poll(*notification).value_or(0), 0x9u);
  KEXPECT_EQ(sys::ipc::poll(*notification).value_or(1), 0u);

  // A signal before the wait is not lost.
  KEXPECT_OK(sys::ipc::signal(*badged));
  KEXPECT_EQ(sys::ipc::wait(*notification).value_or(0), 0x8u);

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *badged));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *notification));
}

KTEST(Notification, wait_sleeps_until_a_signal_comes) {
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(notification);

  struct Waiter {
    CapId notification;
    std::atomic<std::uint64_t> bits{0};
    std::atomic<bool> done{false};
  } waiter{.notification = *notification};

  auto thread = rt::Thread::spawn(
      "waiter",
      [](void* const context) noexcept {
        auto& self = *static_cast<Waiter*>(context);
        self.bits.store(sys::ipc::wait(self.notification).value_or(0));
        self.done.store(true);
      },
      &waiter);
  KREQUIRE_OK(thread);

  (void)sys::time::sleep_for(5'000'000);
  KEXPECT(!waiter.done.load());

  KEXPECT_OK(sys::ipc::signal(*notification));
  KEXPECT_OK(thread->join());
  KEXPECT_EQ(waiter.bits.load(), 1u);
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *notification));
}

KTEST(Endpoint, nothing_waits_with_try_and_nobody_there) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);

  KEXPECT_ERROR(sys::ipc::try_send(*endpoint, Message{LABEL}), Error::WouldBlock);
  KEXPECT_ERROR(sys::ipc::try_recv(*endpoint), Error::WouldBlock);

  // Without Write, nothing goes in; without Read, nothing comes out.
  const auto receiver = sys::cap::copy(NULL_CAP, *endpoint, NULL_CAP, sys::CapRights::Read);
  const auto sender = sys::cap::copy(NULL_CAP, *endpoint, NULL_CAP, sys::CapRights::Write);
  KREQUIRE_OK(receiver);
  KREQUIRE_OK(sender);
  KEXPECT_ERROR(sys::ipc::try_send(*receiver, Message{LABEL}), Error::AccessDenied);
  KEXPECT_ERROR(sys::ipc::try_recv(*sender), Error::AccessDenied);

  for (const CapId id : {*sender, *receiver, *endpoint}) {
    KEXPECT_OK(sys::cap::remove(NULL_CAP, id));
  }
}

KTEST(Endpoint, six_words_go_across_in_registers) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);

  Server server{.endpoint = *endpoint};
  auto thread = rt::Thread::spawn("server", &receive_one, &server);
  KREQUIRE_OK(thread);

  KEXPECT_OK(sys::ipc::send(*endpoint, Message{LABEL, 10, 11, 12, 13, 14, 15}));
  KEXPECT_OK(thread->join());

  KEXPECT(server.error == Error::Success);
  KEXPECT_EQ(server.received.tag.get_label(), LABEL);
  KEXPECT_EQ(server.received.tag.get_length(), 6u);
  KEXPECT_EQ(server.received.tag.get_extra_caps(), 0u);
  KEXPECT(!server.received.tag.has_error());
  for (std::size_t i = 0; i < 6; ++i) {
    KEXPECT_EQ(server.words[i], 10 + i);
  }

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *endpoint));
}

// Words past the sixth come out of the sender's IPC buffer and into the receiver's, which are
// different threads' and so different buffers.
KTEST(Endpoint, long_messages_go_through_each_threads_buffer) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);

  Server server{.endpoint = *endpoint};
  auto thread = rt::Thread::spawn("server", &receive_one, &server);
  KREQUIRE_OK(thread);

  std::array<std::uint64_t, sys::MsgTag::MAX_TOTAL_WORDS> words{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    words[i] = 0xA000 + i;
  }

  KEXPECT_OK(sys::ipc::send_extended(*endpoint, LABEL, words));
  KEXPECT_OK(thread->join());

  KEXPECT(server.error == Error::Success);
  KEXPECT(!server.received.tag.has_error());
  KEXPECT_EQ(server.received.tag.get_length(), words.size());
  KEXPECT_BYTES_EQ(std::span{server.words}, std::span{words});

  // More than fits is refused before anything is sent.
  const std::array<std::uint64_t, sys::MsgTag::MAX_TOTAL_WORDS + 1> too_many{};
  KEXPECT_ERROR(sys::ipc::send_extended(*endpoint, LABEL, too_many), Error::InvalidArgument);

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *endpoint));
}

// A capability sent along lands in the receiver's CSpace, under an ID of its own there.
KTEST(Endpoint, capabilities_go_along_with_a_message) {
  const auto endpoint = sys::cap::create_endpoint();
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(endpoint);
  KREQUIRE_OK(notification);

  Server server{.endpoint = *endpoint};
  auto thread = rt::Thread::spawn("server", &receive_one, &server);
  KREQUIRE_OK(thread);

  const sys::CapTransfer transfer{
      .src_cap = *notification, .rights_mask = sys::CapRights::Write, .new_badge = 0x40};
  KEXPECT_OK(sys::ipc::send_extended(*endpoint, LABEL, std::array<std::uint64_t, 1>{7},
                                     std::span{&transfer, 1}));
  KEXPECT_OK(thread->join());

  KEXPECT(server.error == Error::Success);
  KEXPECT_EQ(server.received.tag.get_extra_caps(), 1u);
  KEXPECT_EQ(server.received_caps, 1u);
  KREQUIRE_NE(server.received_cap, NULL_CAP);
  KEXPECT_NE(server.received_cap, *notification);

  // It has the rights it was sent with, and the badge.
  KEXPECT_OK(sys::ipc::signal(server.received_cap));
  KEXPECT_EQ(sys::ipc::poll(*notification).value_or(0), 0x40u);
  KEXPECT_ERROR(sys::ipc::poll(server.received_cap), Error::AccessDenied);

  for (const CapId id : {server.received_cap, *notification, *endpoint}) {
    KEXPECT_OK(sys::cap::remove(NULL_CAP, id));
  }
}

KTEST(Endpoint, call_and_reply_make_a_round_trip_with_the_callers_badge) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);
  const auto client =
      sys::cap::mint(NULL_CAP, *endpoint, NULL_CAP, sys::CapRights::ClientCall, 0x77);
  KREQUIRE_OK(client);

  Server server{.endpoint = *endpoint, .rounds = 3};
  auto thread = rt::Thread::spawn("server", &answer_calls, &server);
  KREQUIRE_OK(thread);

  for (std::uint64_t round = 0; round < 3; ++round) {
    const auto answer = sys::ipc::call(*client, Message{LABEL, round, 1, 2, 3, 4});
    KREQUIRE_OK(answer);
    KEXPECT_EQ(answer->tag.get_label(), LABEL + 1);
    KEXPECT_EQ(answer->words[0], round + 1);
    KEXPECT_EQ(answer->words[4], 5u);
    KEXPECT_EQ(answer->words[5], 0x77u);
  }

  KEXPECT_OK(thread->join());
  KEXPECT(server.error == Error::Success);
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *client));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *endpoint));
}

// The right to answer can be kept as a capability, and used after the server went on.
KTEST(Endpoint, a_saved_reply_answers_later) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);

  struct Saver {
    CapId endpoint;
    CapId saved{NULL_CAP};
  } saver{.endpoint = *endpoint};

  auto thread = rt::Thread::spawn(
      "saver",
      [](void* const context) noexcept {
        auto& self = *static_cast<Saver*>(context);
        if (sys::ipc::recv(self.endpoint)) {
          self.saved = sys::ipc::save_reply().value_or(NULL_CAP);
        }
      },
      &saver);
  KREQUIRE_OK(thread);

  // The answer comes from this thread, once the saver is gone: the call has to be made by
  // somebody else, so a second thread makes it.
  struct Caller {
    CapId endpoint;
    std::uint64_t answer{0};
  } caller{.endpoint = *endpoint};
  auto calling = rt::Thread::spawn(
      "caller",
      [](void* const context) noexcept {
        auto& self = *static_cast<Caller*>(context);
        self.answer = sys::ipc::call(self.endpoint, Message{LABEL})
                          .transform([](const Message& m) { return m.words[0]; })
                          .value_or(0);
      },
      &caller);
  KREQUIRE_OK(calling);

  KEXPECT_OK(thread->join());
  KREQUIRE_NE(saver.saved, NULL_CAP);
  KEXPECT_OK(sys::ipc::reply(Message{LABEL, 99}, saver.saved));
  KEXPECT_OK(calling->join());
  KEXPECT_EQ(caller.answer, 99u);

  // A reply answers once.
  KEXPECT(!sys::ipc::reply(Message{LABEL, 100}, saver.saved));
  (void)sys::cap::remove(NULL_CAP, saver.saved);
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *endpoint));
}

// A forked child has the endpoint under the same ID, so it can call this process.
KTEST(Endpoint, messages_go_between_processes) {
  const auto endpoint = sys::cap::create_endpoint();
  KREQUIRE_OK(endpoint);

  const auto forked = sys::proc::fork_here("caller");
  KREQUIRE_OK(forked);
  if (forked->is_child()) {
    const auto answer = sys::ipc::call(*endpoint, Message{LABEL, 20});
    (void)sys::proc::exit(answer ? static_cast<int>(answer->words[0]) : -100);
    __builtin_trap();
  }

  const auto request = sys::ipc::recv(*endpoint);
  KEXPECT_OK(request);
  KEXPECT_EQ(request ? request->words[0] : 0, 20u);
  KEXPECT_OK(sys::ipc::reply(Message{LABEL, 21}));

  int status = 0;
  KEXPECT_OK(sys::proc::wait(forked->child_pid, &status));
  KEXPECT_EQ(status, 21);
  KEXPECT_OK(sys::cap::remove(NULL_CAP, forked->child_process));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *endpoint));
}
}  // namespace user::testing
