// IPC: endpoints and the threads that meet on them, calls and their answers, capabilities on the
// way, endpoints that close under waiters, notifications, and a shared ring with doorbells.
#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "core/cspace.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/invoke.hpp"
#include "core/ipc/io_ring.hpp"
#include "core/ipc/notification.hpp"
#include "ktest.hpp"
#include "probe.hpp"
#include "sched/sleep.hpp"
#include "utils/logger.hpp"

namespace kernel::testing {
namespace {
namespace ipc = core::ipc;
using core_probe::Counters;
using core_probe::Probe;
using ipc::CapFlags;
using ipc::CapId;
using ipc::CapRef;
using ipc::CapRights;
using ipc::CSpace;
using ipc::Endpoint;
using ipc::Message;
using ipc::MsgFlags;
using ipc::MsgTag;
using ipc::Notification;
using ipc::ThreadIpcContext;
using klib::Duration;
using support::A_MOMENT;
using support::start;
using support::wait_for;

[[nodiscard]] CapRef<Endpoint> new_endpoint() noexcept {
  auto made = Endpoint::create();
  KEXPECT(made.has_value());
  return made ? std::move(*made) : CapRef<Endpoint>{};
}

[[nodiscard]] CapRef<Notification> new_notification() noexcept {
  auto made = Notification::create();
  KEXPECT(made.has_value());
  return made ? std::move(*made) : CapRef<Notification>{};
}

[[nodiscard]] CapRef<CSpace> new_cspace() noexcept {
  auto made = CSpace::create();
  KEXPECT(made.has_value());
  return made ? std::move(*made) : CapRef<CSpace>{};
}

[[nodiscard]] Error result_of(const std::expected<void, Error>& result) noexcept {
  return result ? Error::Success : result.error();
}

// A thread that receives once, on an endpoint or through a capability, and says what it got.
struct Receiver {
  Endpoint* endpoint{nullptr};
  CSpace* cspace{nullptr};
  CapId cap{};
  ipc::IpcBuffer* buffer{nullptr};
  Notification* bound{nullptr};
  Message got{};
  Error result{Error::Success};
  std::atomic<bool> ready{false};
  std::atomic<bool> done{false};
};

void receive_once(Receiver* r) noexcept {
  ThreadIpcContext ctx{r->cspace, r->buffer};
  if (r->bound != nullptr) {
    KEXPECT(r->bound->bind(ctx).has_value());
  }

  r->ready.store(true, std::memory_order_release);
  r->result = result_of(r->endpoint != nullptr ? r->endpoint->recv(ctx, r->got, CapRights::Read)
                                               : ipc::recv(ctx, r->cap, r->got));
  r->done.store(true, std::memory_order_release);
}
}  // namespace

// Whoever comes first waits; the message, the badge of the sender's capability and all six words
// arrive either way.
KTEST(Ipc, a_sender_and_a_receiver_meet_whoever_comes_first) {
  auto endpoint = new_endpoint();
  KREQUIRE(endpoint);

  Receiver receiver{.endpoint = endpoint.get()};
  KREQUIRE(start("receiver", &receive_once, &receiver) != nullptr);
  KEXPECT(wait_for([&] { return receiver.ready.load(std::memory_order_acquire); }));
  (void)sched::sleep_for(A_MOMENT);

  ThreadIpcContext ctx;
  const Message sent{0x51, 1, 2, 3, 4, 5, 6};
  KEXPECT(endpoint->send(ctx, sent, 0xBAD9E, CapRights::Write).has_value());
  KEXPECT(wait_for([&] { return receiver.done.load(std::memory_order_acquire); }));
  KEXPECT(receiver.result == Error::Success);
  KEXPECT_EQ(receiver.got.tag.get_label(), 0x51u);
  KEXPECT_EQ(receiver.got.badge, 0xBAD9Eu);
  KEXPECT_EQ(receiver.got.regs[5], 6u);
  KEXPECT(!ipc::has_msg_flag(receiver.got.tag.get_flags(), MsgFlags::ReplyRequired));

  // The other way round: the sender waits in the queue until a receiver turns up.
  struct Sender {
    Endpoint* endpoint;
    std::atomic<bool> done{false};
  } sender{.endpoint = endpoint.get()};

  KREQUIRE(start(
               "sender",
               +[](Sender* s) noexcept {
                 ThreadIpcContext own;
                 KEXPECT(s->endpoint->send(own, Message{0x52, 9}, 3, CapRights::Write).has_value());
                 s->done.store(true, std::memory_order_release);
               },
               &sender) != nullptr);

  Message got{};
  KEXPECT(wait_for([&] { return endpoint->try_recv(ctx, got, CapRights::Read).has_value(); }));
  KEXPECT_EQ(got.tag.get_label(), 0x52u);
  KEXPECT_EQ(got.badge, 3u);
  KEXPECT(wait_for([&] { return sender.done.load(std::memory_order_acquire); }));

  // Nobody there: the non-waiting kinds say so.
  KEXPECT(endpoint->try_send(ctx, sent, 0, CapRights::Write).error() == Error::WouldBlock);
  KEXPECT(endpoint->try_recv(ctx, got, CapRights::Read).error() == Error::WouldBlock);

  // Without the rights for it, nothing at all.
  KEXPECT(endpoint->send(ctx, sent, 0, CapRights::Read).error() == Error::AccessDenied);
  KEXPECT(endpoint->recv(ctx, got, CapRights::Write).error() == Error::AccessDenied);
}

// A call waits for its answer, which only the thread that received it can give.
KTEST(Ipc, a_call_waits_for_the_answer) {
  auto endpoint = new_endpoint();
  KREQUIRE(endpoint);

  struct Server {
    Endpoint* endpoint;
    bool saw_reply_required{false};
    std::atomic<bool> done{false};
  } server{.endpoint = endpoint.get()};

  KREQUIRE(start(
               "server",
               +[](Server* s) noexcept {
                 ThreadIpcContext ctx;
                 Message request{};
                 KEXPECT(s->endpoint->recv(ctx, request, CapRights::Read).has_value());
                 s->saw_reply_required =
                     ipc::has_msg_flag(request.tag.get_flags(), MsgFlags::ReplyRequired);
                 KEXPECT(ctx.has_caller());
                 (void)sched::sleep_for(A_MOMENT);  // the caller waits all along
                 KEXPECT(Endpoint::reply(ctx, Message{0x60, request.regs[0] + 1}).has_value());
                 KEXPECT(Endpoint::reply(ctx, Message{0x60}).error() == Error::NotFound);
                 s->done.store(true, std::memory_order_release);
               },
               &server) != nullptr);

  ThreadIpcContext ctx;
  Message answer{};
  KEXPECT(endpoint->call(ctx, Message{0x61, 41}, answer, 0, CapRights::Write).has_value());
  KEXPECT_EQ(answer.tag.get_label(), 0x60u);
  KEXPECT_EQ(answer.regs[0], 42u);
  KEXPECT(wait_for([&] { return server.done.load(std::memory_order_acquire); }));
  KEXPECT(server.saw_reply_required);
}

// Clients call two servers that answer with reply_recv(), every call with its own numbers and
// each client with its own badge. Every answer has to be for its own call.
KTEST(Ipc, servers_answer_many_clients_each_their_own) {
  constexpr std::uint32_t SERVERS = 2;
  constexpr std::uint32_t CLIENTS = 6;
  constexpr std::uint32_t CALLS = 2000;
  constexpr std::uint32_t QUIT = 0xDEAD;

  auto endpoint = new_endpoint();
  KREQUIRE(endpoint);

  struct Shared {
    Endpoint* endpoint;
    std::atomic<std::uint32_t> servers_done{0};
    std::atomic<std::uint32_t> clients_done{0};
    std::atomic<std::uint64_t> wrong{0};
    std::atomic<std::uint64_t> served{0};
  };

  struct Client {
    Shared* shared;
    std::uint64_t id;
  };

  static Shared shared;
  static std::array<Client, CLIENTS> clients;
  shared.endpoint = endpoint.get();
  shared.servers_done.store(0);
  shared.clients_done.store(0);
  shared.wrong.store(0);
  shared.served.store(0);

  for (std::uint32_t i = 0; i < SERVERS; ++i) {
    KREQUIRE(start(
                 "server",
                 +[](Shared* s) noexcept {
                   ThreadIpcContext ctx;
                   Message request{};
                   Message answer{};
                   std::uint64_t local = 0;
                   bool first = true;
                   while (true) {
                     const auto got =
                         first ? s->endpoint->recv(ctx, request, CapRights::Read)
                               : s->endpoint->reply_recv(ctx, answer, request, CapRights::Read);
                     first = false;
                     if (!got || request.tag.get_label() == QUIT) {
                       break;
                     }

                     // The badge says who called.
                     if (request.badge != request.regs[0] + 1) {
                       s->wrong.fetch_add(1, std::memory_order_relaxed);
                     }

                     answer = Message{0x71, request.regs[0], request.regs[1] * 3 + 1};
                     ++local;
                   }

                   s->served.fetch_add(local, std::memory_order_relaxed);
                   s->servers_done.fetch_add(1, std::memory_order_release);
                 },
                 &shared) != nullptr);
  }

  for (std::uint32_t i = 0; i < CLIENTS; ++i) {
    clients[i] = Client{.shared = &shared, .id = i};
    KREQUIRE(start(
                 "client",
                 +[](Client* c) noexcept {
                   ThreadIpcContext ctx;
                   for (std::uint64_t n = 0; n < CALLS; ++n) {
                     Message answer{};
                     const auto called = c->shared->endpoint->call(
                         ctx, Message{0x70, c->id, n}, answer, c->id + 1, CapRights::Write);
                     if (!called || answer.regs[0] != c->id || answer.regs[1] != n * 3 + 1) {
                       c->shared->wrong.fetch_add(1, std::memory_order_relaxed);
                     }
                   }

                   c->shared->clients_done.fetch_add(1, std::memory_order_release);
                 },
                 &clients[i]) != nullptr);
  }

  KEXPECT(wait_for([&] { return shared.clients_done.load(std::memory_order_acquire) == CLIENTS; }));

  ThreadIpcContext ctx;
  for (std::uint32_t i = 0; i < SERVERS; ++i) {
    KEXPECT(endpoint->send(ctx, Message{QUIT}, 0, CapRights::Write).has_value());
  }

  KEXPECT(wait_for([&] { return shared.servers_done.load(std::memory_order_acquire) == SERVERS; }));
  KEXPECT_EQ(shared.wrong.load(), 0u);
  KEXPECT_EQ(shared.served.load(), std::uint64_t{CLIENTS} * CALLS);
  KEXPECT_EQ(endpoint->delivered(), std::uint64_t{CLIENTS} * CALLS + SERVERS);
}

// Words past the registers come from the sender's buffer, and capabilities from its CSpace:
// copied with fewer rights, moved, or minted with a badge. Without Grant, none of them.
KTEST(Ipc, long_messages_and_capabilities_go_through_the_buffers) {
  Counters counters;
  auto from = new_cspace();
  auto to = new_cspace();
  auto endpoint = new_endpoint();
  auto first = Probe::create(counters);
  auto second = Probe::create(counters);
  KREQUIRE(from && to && endpoint && first && second);

  const auto copied = from->insert(**first, CapRights::All);
  const auto moved = from->insert(**second, CapRights::All);
  KREQUIRE(copied && moved);

  static ipc::IpcBuffer sent_buffer;
  static ipc::IpcBuffer got_buffer;
  sent_buffer = {};
  got_buffer = {};
  for (std::uint32_t i = 0; i < 20; ++i) {
    sent_buffer.overflow_words[i] = 1000 + i;
  }

  sent_buffer.send_caps[0] = {.src_cap = *copied, .rights_mask = CapRights::Read};
  sent_buffer.send_caps[1] = {.src_cap = *moved, .move_cap = 1};
  sent_buffer.send_caps[2] = {
      .src_cap = *copied, .rights_mask = CapRights::ReadWrite, .new_badge = 77};

  Receiver receiver{.endpoint = endpoint.get(), .cspace = to.get(), .buffer = &got_buffer};
  KREQUIRE(start("receiver", &receive_once, &receiver) != nullptr);
  KEXPECT(wait_for([&] { return receiver.ready.load(std::memory_order_acquire); }));

  ThreadIpcContext ctx{from.get(), &sent_buffer};
  Message message{0x80, 1, 2, 3, 4, 5, 6};
  message.tag = MsgTag::make(0x80, MsgTag::MAX_INLINE_WORDS + 20, 3);
  KEXPECT(endpoint->send(ctx, message, 0, CapRights::Write | CapRights::Grant).has_value());
  KEXPECT(wait_for([&] { return receiver.done.load(std::memory_order_acquire); }));

  const MsgTag& tag = receiver.got.tag;
  KEXPECT(!ipc::has_msg_flag(tag.get_flags(), MsgFlags::KernelError));
  KEXPECT_EQ(tag.get_length(), MsgTag::MAX_INLINE_WORDS + 20u);
  KEXPECT_EQ(got_buffer.overflow_words[19], 1019u);
  KEXPECT_EQ(got_buffer.received_cap_count, 3u);

  {
    const utils::rcu::ReadGuard rcu;
    const auto one = to->lookup(got_buffer.recv_caps[0], rcu);
    const auto two = to->lookup(got_buffer.recv_caps[1], rcu);
    const auto three = to->lookup(got_buffer.recv_caps[2], rcu);
    KEXPECT(one && one->rights() == CapRights::Read && one->object() == first->get());
    KEXPECT(two && ipc::has_cap_flag(two->flags(), CapFlags::Original) &&
            two->object() == second->get());
    KEXPECT(three && three->rights() == CapRights::ReadWrite && three->badge == 77);
    KEXPECT(!from->lookup(*moved, rcu).has_value());
  }

  // Both probes count their end in this frame, so it has to wait for them.
  ctx.set_cspace(nullptr);
  from.reset();
  to.reset();
  endpoint.reset();
  first->reset();
  second->reset();
  KEXPECT(core_probe::settle(counters, 2));
}

KTEST(Ipc, capabilities_need_grant_and_an_answer_needs_grant_reply) {
  Counters counters;
  auto from = new_cspace();
  auto to = new_cspace();
  auto endpoint = new_endpoint();
  auto probe = Probe::create(counters);
  KREQUIRE(from && to && endpoint && probe);

  const auto cap = from->insert(**probe, CapRights::All);
  KREQUIRE(cap.has_value());

  static ipc::IpcBuffer client_buffer;
  static ipc::IpcBuffer server_buffer;
  client_buffer = {};
  server_buffer = {};
  client_buffer.send_caps[0] = {.src_cap = *cap};

  // Without Grant on the endpoint: the message gets through, the capability does not.
  Receiver receiver{.endpoint = endpoint.get(), .cspace = to.get(), .buffer = &server_buffer};
  KREQUIRE(start("receiver", &receive_once, &receiver) != nullptr);
  KEXPECT(wait_for([&] { return receiver.ready.load(std::memory_order_acquire); }));

  ThreadIpcContext ctx{from.get(), &client_buffer};
  Message message{0x90};
  message.tag = MsgTag::make(0x90, 0, 1);
  KEXPECT(endpoint->send(ctx, message, 0, CapRights::Write).has_value());
  KEXPECT(wait_for([&] { return receiver.done.load(std::memory_order_acquire); }));
  KEXPECT(ipc::has_msg_flag(receiver.got.tag.get_flags(), MsgFlags::KernelError));
  KEXPECT(receiver.got.tag.error() == Error::AccessDenied);
  KEXPECT_EQ(server_buffer.received_cap_count, 0u);
  KEXPECT_EQ(to->active_count(), 0u);

  // An answer carries capabilities back only for a caller whose capability had GrantReply.
  struct Server {
    Endpoint* endpoint;
    CSpace* cspace;
    ipc::IpcBuffer* buffer;
    CapId cap;
    std::atomic<std::uint32_t> answered{0};
  } server{.endpoint = endpoint.get(), .cspace = to.get(), .buffer = &server_buffer};

  const auto server_cap = to->insert(**probe, CapRights::Read);
  KREQUIRE(server_cap.has_value());
  server_buffer.send_caps[0] = {.src_cap = *server_cap};

  KREQUIRE(start(
               "server",
               +[](Server* s) noexcept {
                 ThreadIpcContext own{s->cspace, s->buffer};
                 for (int i = 0; i < 2; ++i) {
                   Message request{};
                   KEXPECT(s->endpoint->recv(own, request, CapRights::Read).has_value());
                   Message answer{0x91};
                   answer.tag = MsgTag::make(0x91, 0, 1);
                   KEXPECT(Endpoint::reply(own, answer).has_value());
                   s->answered.fetch_add(1, std::memory_order_release);
                 }
               },
               &server) != nullptr);

  client_buffer = {};
  Message answer{};
  KEXPECT(endpoint->call(ctx, Message{0x92}, answer, 0, CapRights::Write).has_value());
  KEXPECT(answer.tag.error() == Error::AccessDenied);
  KEXPECT_EQ(client_buffer.received_cap_count, 0u);

  KEXPECT(endpoint->call(ctx, Message{0x92}, answer, 0, CapRights::Write | CapRights::GrantReply)
              .has_value());
  KEXPECT(!ipc::has_msg_flag(answer.tag.get_flags(), MsgFlags::KernelError));
  KEXPECT_EQ(client_buffer.received_cap_count, 1u);
  KEXPECT(wait_for([&] { return server.answered.load(std::memory_order_acquire) == 2; }));

  ctx.set_cspace(nullptr);
  from.reset();
  to.reset();
  endpoint.reset();
  probe->reset();
  KEXPECT(core_probe::settle(counters, 1));
}

// A saved reply lets the server take other calls first and answer later, through a capability.
// Dropping that capability, or receiving again without answering, ends the call with NotFound.
KTEST(Ipc, a_saved_reply_is_answered_later_and_a_dropped_one_fails_the_call) {
  auto endpoint = new_endpoint();
  auto server_space = new_cspace();
  KREQUIRE(endpoint && server_space);

  struct Caller {
    Endpoint* endpoint;
    std::uint64_t value;
    Message answer{};
    Error result{Error::Success};
    std::atomic<bool> done{false};
  };

  const auto caller = [](Caller* c) noexcept {
    ThreadIpcContext ctx;
    c->result =
        result_of(c->endpoint->call(ctx, Message{0xA0, c->value}, c->answer, 0, CapRights::Write));
    c->done.store(true, std::memory_order_release);
  };

  Caller first{.endpoint = endpoint.get(), .value = 1};
  Caller second{.endpoint = endpoint.get(), .value = 2};
  Caller dropped{.endpoint = endpoint.get(), .value = 3};
  Caller skipped{.endpoint = endpoint.get(), .value = 4};

  ThreadIpcContext ctx{server_space.get()};
  Message request{};

  KREQUIRE(start("first", +caller, &first) != nullptr);
  KEXPECT(endpoint->recv(ctx, request, CapRights::Read).has_value());
  KEXPECT_EQ(request.regs[0], 1u);
  const auto saved = Endpoint::save_reply(ctx);
  KREQUIRE(saved.has_value());
  KEXPECT(!ctx.has_caller());

  KREQUIRE(start("second", +caller, &second) != nullptr);
  KEXPECT(endpoint->recv(ctx, request, CapRights::Read).has_value());
  KEXPECT_EQ(request.regs[0], 2u);
  KEXPECT(Endpoint::reply(ctx, Message{0xA1, 20}).has_value());
  KEXPECT(wait_for([&] { return second.done.load(std::memory_order_acquire); }));
  KEXPECT(!first.done.load(std::memory_order_acquire));

  KEXPECT(Endpoint::reply_with(ctx, *saved, Message{0xA1, 10}).has_value());
  KEXPECT(wait_for([&] { return first.done.load(std::memory_order_acquire); }));
  KEXPECT(first.result == Error::Success && first.answer.regs[0] == 10);
  KEXPECT(second.result == Error::Success && second.answer.regs[0] == 20);
  KEXPECT(Endpoint::reply_with(ctx, *saved, Message{0xA1}).error() == Error::NotFound);

  // A reply capability that goes without an answer.
  KREQUIRE(start("dropped", +caller, &dropped) != nullptr);
  KEXPECT(endpoint->recv(ctx, request, CapRights::Read).has_value());
  const auto to_drop = Endpoint::save_reply(ctx);
  KREQUIRE(to_drop.has_value());
  KEXPECT(server_space->copy(*to_drop, *server_space).error() == Error::InvalidArgument);
  KEXPECT(server_space->remove(*to_drop).has_value());
  KEXPECT(wait_for([&] {
    core::ipc::reclaim_now();
    return dropped.done.load(std::memory_order_acquire);
  }));
  KEXPECT(dropped.result == Error::NotFound);

  // A server that receives again without answering.
  KREQUIRE(start("skipped", +caller, &skipped) != nullptr);
  KEXPECT(endpoint->recv(ctx, request, CapRights::Read).has_value());
  KEXPECT(endpoint->try_recv(ctx, request, CapRights::Read).error() == Error::WouldBlock);
  KEXPECT(wait_for([&] { return skipped.done.load(std::memory_order_acquire); }));
  KEXPECT(skipped.result == Error::NotFound);
}

// A thread waits in an endpoint through a capability. When the last capability goes, nobody could
// ever come, so it is woken with an error, and the endpoint takes nobody from then on.
KTEST(Ipc, waiters_are_woken_when_the_last_capability_goes) {
  auto cspace = new_cspace();
  auto endpoint = new_endpoint();
  KREQUIRE(cspace && endpoint);

  const auto cap = cspace->insert(*endpoint, CapRights::ReadWrite);
  KREQUIRE(cap.has_value());

  Receiver receiver{.cspace = cspace.get(), .cap = *cap};
  KREQUIRE(start("receiver", &receive_once, &receiver) != nullptr);
  KEXPECT(wait_for([&] { return receiver.ready.load(std::memory_order_acquire); }));
  (void)sched::sleep_for(A_MOMENT);
  KEXPECT(!receiver.done.load(std::memory_order_acquire));

  KEXPECT(cspace->remove(*cap).has_value());
  KEXPECT(wait_for([&] { return receiver.done.load(std::memory_order_acquire); }));
  KEXPECT(receiver.result == Error::InvalidState);

  ThreadIpcContext ctx;
  KEXPECT(endpoint->send(ctx, Message{1}, 0, CapRights::Write).error() == Error::InvalidState);
}

// What the capability says decides what is allowed through it, and its badge is what arrives.
KTEST(Ipc, invocations_go_by_the_capability) {
  auto cspace = new_cspace();
  auto endpoint = new_endpoint();
  auto notification = new_notification();
  KREQUIRE(cspace && endpoint && notification);

  const auto read_only = cspace->insert(*endpoint, CapRights::Read);
  const auto write_only = cspace->insert(*endpoint, CapRights::Write, CapFlags::Original, 0x44);
  const auto signaller = cspace->insert(*notification, CapRights::Write, CapFlags::Original, 0x30);
  const auto waiter = cspace->insert(*notification, CapRights::Read);
  KREQUIRE(read_only && write_only && signaller && waiter);

  ThreadIpcContext ctx{cspace.get()};
  Message message{};
  KEXPECT(ipc::send(ctx, *read_only, Message{1}).error() == Error::AccessDenied);
  KEXPECT(ipc::recv(ctx, *write_only, message).error() == Error::AccessDenied);
  KEXPECT(ipc::wait(ctx, *signaller).error() == Error::AccessDenied);
  KEXPECT(ipc::signal(ctx, *waiter).error() == Error::AccessDenied);
  KEXPECT(ipc::send(ctx, *signaller, Message{1}).error() == Error::InvalidArgument);  // not one

  KEXPECT(ipc::signal(ctx, *signaller).has_value());
  const auto bits = ipc::poll(ctx, *waiter);
  KEXPECT(bits && *bits == 0x30);

  Receiver receiver{.cspace = cspace.get(), .cap = *read_only};
  KREQUIRE(start("receiver", &receive_once, &receiver) != nullptr);
  KEXPECT(wait_for([&] { return receiver.ready.load(std::memory_order_acquire); }));
  KEXPECT(ipc::send(ctx, *write_only, Message{0xB0}).has_value());
  KEXPECT(wait_for([&] { return receiver.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(receiver.got.badge, 0x44u);
}

KTEST(Notification, signals_add_up_and_a_wait_takes_them_all) {
  auto notification = new_notification();
  KREQUIRE(notification);

  notification->signal(1);
  notification->signal(2);
  notification->signal(4);
  notification->signal(0);  // nothing
  KEXPECT_EQ(notification->peek(), 7u);
  const auto bits = notification->wait();
  KEXPECT(bits && *bits == 7);
  KEXPECT_EQ(notification->poll(), 0u);

  const auto nothing = notification->wait_for(Duration::milliseconds(5));
  KEXPECT(!nothing && nothing.error() == Error::Timeout);
}

// Many waiters, signals one at a time: every signal wakes someone, and nobody is left waiting.
KTEST(Notification, every_waiter_gets_a_turn) {
  constexpr std::uint32_t WAITERS = 4;
  auto notification = new_notification();
  KREQUIRE(notification);

  struct Shared {
    Notification* notification;
    std::atomic<std::uint32_t> ready{0};
    std::atomic<std::uint32_t> woken{0};
    std::atomic<std::uint64_t> bits{0};
  } shared{.notification = notification.get()};

  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    KREQUIRE(start(
                 "waiter",
                 +[](Shared* s) noexcept {
                   s->ready.fetch_add(1, std::memory_order_release);
                   if (const auto got = s->notification->wait()) {
                     s->bits.fetch_or(*got, std::memory_order_relaxed);
                   }

                   s->woken.fetch_add(1, std::memory_order_release);
                 },
                 &shared) != nullptr);
  }

  KEXPECT(wait_for([&] { return shared.ready.load(std::memory_order_acquire) == WAITERS; }));
  for (std::uint32_t i = 0; i < WAITERS; ++i) {
    (void)sched::sleep_for(A_MOMENT);
    notification->signal(std::uint64_t{1} << i);
    KEXPECT(wait_for([&] { return shared.woken.load(std::memory_order_acquire) >= i + 1; }));
  }

  KEXPECT(wait_for([&] { return shared.woken.load(std::memory_order_acquire) == WAITERS; }));
  KEXPECT_EQ(shared.bits.load(), (1u << WAITERS) - 1);
}

// A thread with a bound notification waits for a message and a signal at once: a signal ends the
// receive with the bits, and the next receive gets the message.
KTEST(Notification, a_bound_thread_hears_a_signal_while_it_waits_for_a_message) {
  auto endpoint = new_endpoint();
  auto notification = new_notification();
  KREQUIRE(endpoint && notification);

  struct Scene {
    Endpoint* endpoint;
    Notification* notification;
    Message first{};
    Message second{};
    std::atomic<bool> ready{false};
    std::atomic<bool> first_done{false};
    std::atomic<bool> done{false};
  } scene{.endpoint = endpoint.get(), .notification = notification.get()};

  KREQUIRE(start(
               "bound",
               +[](Scene* s) noexcept {
                 ThreadIpcContext ctx;
                 KEXPECT(s->notification->bind(ctx).has_value());
                 s->ready.store(true, std::memory_order_release);
                 KEXPECT(s->endpoint->recv(ctx, s->first, CapRights::Read).has_value());
                 s->first_done.store(true, std::memory_order_release);
                 KEXPECT(s->endpoint->recv(ctx, s->second, CapRights::Read).has_value());
                 s->done.store(true, std::memory_order_release);
               },
               &scene) != nullptr);

  KEXPECT(wait_for([&] { return scene.ready.load(std::memory_order_acquire); }));
  (void)sched::sleep_for(A_MOMENT);
  notification->signal(0x30);
  KEXPECT(wait_for([&] { return scene.first_done.load(std::memory_order_acquire); }));
  KEXPECT(ipc::has_msg_flag(scene.first.tag.get_flags(), MsgFlags::IsAsyncNotification));
  KEXPECT_EQ(scene.first.badge, 0x30u);

  ThreadIpcContext ctx;
  (void)sched::sleep_for(A_MOMENT);
  KEXPECT(endpoint->send(ctx, Message{0xC0, 5}, 0, CapRights::Write).has_value());
  KEXPECT(wait_for([&] { return scene.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(scene.second.tag.get_label(), 0xC0u);
  KEXPECT(!ipc::has_msg_flag(scene.second.tag.get_flags(), MsgFlags::IsAsyncNotification));

  // Gone with its context, the binding let go of its reference.
  KEXPECT(wait_for([&] { return notification->ref_count() == 1; }));
}

// A bound receiver while a sender and a signaller go at it together. A signal and a sender can get
// to the receiver at the same moment; whichever wins, nothing the other brought is lost: every
// message arrives, in order, and every bit is seen.
KTEST(Notification, a_bound_receiver_loses_neither_messages_nor_signals) {
  if (hw::smp::online_cpus() < 3) {
    KTEST_SKIP("takes three CPUs");
  }

  constexpr std::uint64_t MESSAGES = 4000;
  constexpr std::uint64_t SIGNALS = 4000;
  auto endpoint = new_endpoint();
  auto notification = new_notification();
  KREQUIRE(endpoint && notification);

  struct Scene {
    Endpoint* endpoint;
    Notification* notification;
    std::atomic<bool> ready{false};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> signaller_done{false};
    std::atomic<bool> done{false};
    std::uint64_t messages{0};
    std::uint64_t out_of_order{0};
    std::uint64_t bits{0};
    std::uint64_t wakeups{0};
  } scene{.endpoint = endpoint.get(), .notification = notification.get()};

  KREQUIRE(start(
               "bound",
               +[](Scene* s) noexcept {
                 ThreadIpcContext ctx;
                 KEXPECT(s->notification->bind(ctx).has_value());
                 s->ready.store(true, std::memory_order_release);
                 while (s->messages < MESSAGES || s->bits != ~std::uint64_t{0}) {
                   Message got{};
                   if (!s->endpoint->recv(ctx, got, CapRights::Read)) {
                     break;
                   }

                   if (ipc::has_msg_flag(got.tag.get_flags(), MsgFlags::IsAsyncNotification)) {
                     s->bits |= got.badge;
                     ++s->wakeups;
                   } else {
                     s->out_of_order += got.regs[0] == s->messages ? 0 : 1;
                     ++s->messages;
                   }
                 }

                 s->done.store(true, std::memory_order_release);
               },
               &scene, support::pinned_to(1)) != nullptr);

  KEXPECT(wait_for([&] { return scene.ready.load(std::memory_order_acquire); }));
  KREQUIRE(start(
               "sender",
               +[](Scene* s) noexcept {
                 ThreadIpcContext ctx;
                 for (std::uint64_t n = 0; n < MESSAGES; ++n) {
                   KEXPECT(
                       s->endpoint->send(ctx, Message{0xD0, n}, 0, CapRights::Write).has_value());
                 }

                 s->sender_done.store(true, std::memory_order_release);
               },
               &scene, support::pinned_to(2)) != nullptr);

  // Every bit at least once, many times over, from here on this CPU.
  for (std::uint64_t n = 0; n < SIGNALS; ++n) {
    scene.notification->signal(std::uint64_t{1} << (n % 64));
    if (n % 16 == 0) {
      sched::Scheduler::yield();
    }
  }

  KEXPECT(wait_for([&] { return scene.done.load(std::memory_order_acquire); }));
  KEXPECT(wait_for([&] { return scene.sender_done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(scene.messages, MESSAGES);
  KEXPECT_EQ(scene.out_of_order, 0u);
  KEXPECT_EQ(scene.bits, ~std::uint64_t{0});
  utils::logger::info("Notification: {} messages and {} wakeups for signals\n", scene.messages,
                      scene.wakeups);
}

// An endpoint that closes while threads keep sending, calling and receiving on it. Every one of
// them comes back, with what it was doing done or an error, and none waits forever.
KTEST(Ipc, an_endpoint_closing_under_traffic_lets_everyone_go) {
  // The traffic never lets up, so it stays off the CPU the test runs on, which it would have to
  // itself to get anything done.
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes a second CPU");
  }

  constexpr std::uint32_t THREADS = 6;
  auto cspace = new_cspace();
  auto endpoint = new_endpoint();
  KREQUIRE(cspace && endpoint);

  const auto cap = cspace->insert(*endpoint, CapRights::ReadWrite);
  KREQUIRE(cap.has_value());

  struct Shared {
    CSpace* cspace;
    CapId cap;
    std::atomic<std::uint32_t> running{0};
    std::atomic<std::uint32_t> finished{0};
    std::atomic<std::uint64_t> operations{0};
  };

  struct Role {
    Shared* shared;
    std::uint32_t kind;
  };

  static Shared shared;
  static std::array<Role, THREADS> roles;
  shared.cspace = cspace.get();
  shared.cap = *cap;
  shared.running.store(0);
  shared.finished.store(0);
  shared.operations.store(0);

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    roles[i] = Role{.shared = &shared, .kind = i % 3};
    KREQUIRE(start(
                 "traffic",
                 +[](Role* r) noexcept {
                   ThreadIpcContext ctx{r->shared->cspace};
                   r->shared->running.fetch_add(1, std::memory_order_release);
                   std::uint64_t local = 0;
                   while (true) {
                     Message message{0xE0, local};
                     Message answer{};
                     std::expected<void, Error> result{};
                     switch (r->kind) {
                       case 0:
                         result = ipc::send(ctx, r->shared->cap, message);
                         break;
                       case 1:
                         result = ipc::call(ctx, r->shared->cap, message, answer);
                         break;
                       default:
                         result = ipc::recv(ctx, r->shared->cap, answer);
                         if (result && ctx.has_caller()) {
                           (void)ipc::reply(ctx, Message{0xE1});
                         }
                         break;
                     }

                     // Receivers answer every call, so any error is the end: the endpoint
                     // closed, or the capability went.
                     if (!result) {
                       break;
                     }

                     ++local;
                   }

                   r->shared->operations.fetch_add(local, std::memory_order_relaxed);
                   r->shared->finished.fetch_add(1, std::memory_order_release);
                 },
                 &roles[i], support::pinned_to(1 + i % (hw::smp::online_cpus() - 1))) != nullptr);
  }

  KEXPECT(wait_for([&] { return shared.running.load(std::memory_order_acquire) == THREADS; }));
  (void)sched::sleep_for(Duration::milliseconds(20));
  KEXPECT(cspace->remove(*cap).has_value());

  KEXPECT(wait_for([&] { return shared.finished.load(std::memory_order_acquire) == THREADS; }));
  KEXPECT_GT(shared.operations.load(), 0u);
}

KTEST(Notification, waiters_are_woken_when_the_last_capability_goes) {
  auto cspace = new_cspace();
  auto notification = new_notification();
  KREQUIRE(cspace && notification);

  const auto cap = cspace->insert(*notification, CapRights::Read);
  KREQUIRE(cap.has_value());

  struct Waiter {
    CSpace* cspace;
    CapId cap;
    Error result{Error::Success};
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
  } waiter{.cspace = cspace.get(), .cap = *cap};

  KREQUIRE(start(
               "waiter",
               +[](Waiter* w) noexcept {
                 ThreadIpcContext ctx{w->cspace};
                 w->ready.store(true, std::memory_order_release);
                 const auto got = ipc::wait(ctx, w->cap);
                 w->result = got ? Error::Success : got.error();
                 w->done.store(true, std::memory_order_release);
               },
               &waiter) != nullptr);

  KEXPECT(wait_for([&] { return waiter.ready.load(std::memory_order_acquire); }));
  (void)sched::sleep_for(A_MOMENT);
  KEXPECT(cspace->remove(*cap).has_value());
  KEXPECT(wait_for([&] { return waiter.done.load(std::memory_order_acquire); }));
  KEXPECT(waiter.result == Error::InvalidState);
}

// A client and a driver on a shared ring, in turns and in bursts. The doorbells only ring for a
// side that sleeps; if one ever stayed silent when it should have, the other would sleep forever.
KTEST(IoRing, nothing_is_left_in_a_ring_with_its_consumer_asleep) {
  constexpr std::uint64_t REQUESTS = 20000;
  constexpr std::uint64_t QUIT = 0;
  using Ring = ipc::SharedIoRing<std::uint64_t, std::uint64_t, 16>;

  auto driver_bell = new_notification();
  auto client_bell = new_notification();
  KREQUIRE(driver_bell && client_bell);

  static Ring ring;
  ring.driver_needs_wakeup.store(false);
  ring.client_needs_wakeup.store(false);

  struct Driver {
    Ring* ring;
    Notification* in;
    Notification* out;
    std::atomic<std::uint64_t> handled{0};
    std::atomic<bool> done{false};
  } driver{.ring = &ring, .in = driver_bell.get(), .out = client_bell.get()};

  KREQUIRE(start(
               "driver",
               +[](Driver* d) noexcept {
                 std::uint64_t request = 0;
                 while (d->ring->next_request(request, *d->in).has_value() && request != QUIT) {
                   while (!d->ring->complete(request * 2, d->out)) {
                     sched::Scheduler::yield();
                   }

                   d->handled.fetch_add(1, std::memory_order_relaxed);
                 }

                 d->done.store(true, std::memory_order_release);
               },
               &driver) != nullptr);

  std::uint64_t wrong = 0;
  std::uint64_t next_expected = 1;
  std::uint64_t sent = 0;
  support::Rng rng{0x10};
  while (next_expected <= REQUESTS) {
    // A burst of up to the ring's size, or one at a time.
    const std::uint64_t burst = rng.below(2) == 0 ? 1 : 1 + rng.below(16);
    for (std::uint64_t i = 0; i < burst && sent < REQUESTS; ++i) {
      while (!ring.submit(sent + 1, driver_bell.get())) {
        sched::Scheduler::yield();
      }

      ++sent;
    }

    while (next_expected <= sent) {
      std::uint64_t response = 0;
      KREQUIRE(ring.next_response(response, *client_bell).has_value());
      wrong += response == next_expected * 2 ? 0 : 1;
      ++next_expected;
    }
  }

  while (!ring.submit(QUIT, driver_bell.get())) {
    sched::Scheduler::yield();
  }

  KEXPECT(wait_for([&] { return driver.done.load(std::memory_order_acquire); }));
  KEXPECT_EQ(wrong, 0u);
  KEXPECT_EQ(driver.handled.load(), REQUESTS);
}
}  // namespace kernel::testing
