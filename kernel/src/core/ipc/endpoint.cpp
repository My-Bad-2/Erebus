#include "core/ipc/endpoint.hpp"

#include <algorithm>
#include <utility>

#include "core/ipc/notification.hpp"
#include "hal/smp/percpu.hpp"
#include "proc/process.hpp"
#include "sched/scheduler.hpp"
#include "utils/assert.hpp"

namespace kernel::core::ipc {
namespace {
using Guard = utils::PreemptGuard<utils::QSpinlock>;

// The bits of @p ctx's bound notification, taken if @p take, or just looked at. Whoever binds may
// be another thread, so the notification is only looked at in a read section.
[[nodiscard]] std::uint64_t bound_bits(const ThreadIpcContext& ctx, bool take) noexcept {
  const utils::rcu::ReadGuard rcu;
  Notification* bound = ctx.bound_notification();
  if (bound == nullptr) {
    return 0;
  }

  return take ? bound->poll() : bound->peek();
}

// One capability transfer, from the sender's CSpace to the receiver's.
[[nodiscard]] std::expected<CapId, Error> transfer(CSpace& from, CSpace& to,
                                                   const CapTransferItem& item) noexcept {
  if (item.move_cap != 0) {
    return from.move(item.src_cap, to);
  }

  if (item.new_badge != 0) {
    // The rights of the source, less what the mask takes away.
    CapRights rights;
    {
      const utils::rcu::ReadGuard rcu;
      const auto entry = from.lookup(item.src_cap, rcu);
      if (!entry) {
        return std::unexpected(entry.error());
      }

      rights = entry->rights() & item.rights_mask;
    }

    return from.mint(item.src_cap, to, rights, item.new_badge);
  }

  return from.copy(item.src_cap, to, item.rights_mask);
}
}  // namespace

ThreadIpcContext::ThreadIpcContext(CSpace* const cspace, IpcBuffer* const buffer) noexcept
    : ThreadIpcContext(sched::current(), cspace, buffer) {}

ThreadIpcContext::ThreadIpcContext(sched::Thread* const thread, CSpace* const cspace,
                                   IpcBuffer* const buffer) noexcept
    : m_thread(thread),
      m_cspace(CapRef<CSpace>::share(cspace)),
      m_buffer(buffer),
      m_kernel_buffer(buffer) {}

ThreadIpcContext::~ThreadIpcContext() noexcept {
  KASSERT(m_queued_on == nullptr);
  abandon();
}

void ThreadIpcContext::abandon() noexcept {
  Endpoint::drop_caller(*this);

  const utils::rcu::ReadGuard rcu;
  if (Notification* bound = m_bound.load(std::memory_order_acquire)) {
    bound->unbind(*this);
  }
}

void ThreadIpcContext::set_cspace(CSpace* const cspace) noexcept {
  m_cspace = CapRef<CSpace>::share(cspace);
}

Endpoint::~Endpoint() noexcept {
  // Every waiter holds a reference, so nobody can be left.
  KASSERT(m_head == nullptr);
}

void Endpoint::push_locked(ThreadIpcContext& ctx) noexcept {
  KASSERT(ctx.m_queued_on == nullptr);
  ctx.m_queued_on = this;
  ctx.m_next = nullptr;
  ctx.m_prev = m_tail;
  (m_tail != nullptr ? m_tail->m_next : m_head) = &ctx;
  m_tail = &ctx;
}

void Endpoint::unlink_locked(ThreadIpcContext& ctx) noexcept {
  KASSERT(ctx.m_queued_on == this);
  (ctx.m_prev != nullptr ? ctx.m_prev->m_next : m_head) = ctx.m_next;
  (ctx.m_next != nullptr ? ctx.m_next->m_prev : m_tail) = ctx.m_prev;
  ctx.m_prev = nullptr;
  ctx.m_next = nullptr;
  ctx.m_queued_on = nullptr;
}

ThreadIpcContext* Endpoint::pop_locked() noexcept {
  ThreadIpcContext* ctx = m_head;
  if (ctx != nullptr) {
    unlink_locked(*ctx);
  }

  return ctx;
}

void Endpoint::finish(ThreadIpcContext& ctx, Error result) noexcept {
  // Only an operation someone took over ends this way, and only once.
  KASSERT(ctx.m_wait.load(std::memory_order_relaxed) == IpcWait::Matched ||
          ctx.m_wait.load(std::memory_order_relaxed) == IpcWait::AwaitingReply);
  KASSERT(ctx.m_queued_on == nullptr);

  // The thread is kept until it is woken: once it sees Done it may return and be gone.
  hw::percpu::preempt_disable();
  sched::WakeList wakes;
  wakes.add(*ctx.m_thread);
  ctx.m_result = result;
  ctx.m_wait.store(IpcWait::Done, std::memory_order_seq_cst);
  wakes.wake_all();
  hw::percpu::preempt_enable();
}

void Endpoint::drop_caller(ThreadIpcContext& ctx) noexcept {
  if (ThreadIpcContext* caller = std::exchange(ctx.m_reply_to, nullptr)) {
    finish(*caller, Error::NotFound);
  }
}

void Endpoint::deliver(const ThreadIpcContext& sender, const Message& message, std::uint64_t badge,
                       bool may_grant, bool is_call, const ThreadIpcContext& receiver,
                       Message& into) noexcept {
  into.tag = message.tag;
  into.badge = badge;
  into.regs = message.regs;
  if (is_call) {
    into.tag.set_flags(into.tag.get_flags() | MsgFlags::ReplyRequired);
  }

  // The words past the registers, as far as both sides have room for them.
  std::uint8_t length = std::min(message.tag.get_length(), MsgTag::MAX_TOTAL_WORDS);
  if (length > MsgTag::MAX_INLINE_WORDS) {
    if (sender.m_buffer != nullptr && receiver.m_buffer != nullptr) {
      std::copy_n(sender.m_buffer->overflow_words.begin(), length - MsgTag::MAX_INLINE_WORDS,
                  receiver.m_buffer->overflow_words.begin());
    } else {
      length = MsgTag::MAX_INLINE_WORDS;
      into.tag.fail(Error::OutOfBounds);
    }
  }

  into.tag.set_length(length);

  const std::uint8_t caps = std::min(message.tag.get_extra_caps(), MsgTag::MAX_EXTRA_CAPS);
  if (receiver.m_buffer != nullptr) {
    receiver.m_buffer->received_cap_count = 0;
  }

  if (caps == 0) {
    return;
  }

  if (!may_grant || sender.m_buffer == nullptr || receiver.m_buffer == nullptr ||
      sender.cspace() == nullptr || receiver.cspace() == nullptr) [[unlikely]] {
    into.tag.set_extra_caps(0);
    into.tag.fail(Error::AccessDenied);
    return;
  }

  std::uint8_t installed = 0;
  for (std::uint8_t i = 0; i < caps; ++i) {
    const CapTransferItem& item = sender.m_buffer->send_caps[i];
    CapId& slot = receiver.m_buffer->recv_caps[i];
    slot = INVALID_CAP_ID;
    if (item.src_cap.is_null()) {
      continue;
    }

    if (const auto granted = transfer(*sender.cspace(), *receiver.cspace(), item)) [[likely]] {
      slot = *granted;
      ++installed;
    } else {
      into.tag.fail(granted.error());
    }
  }

  receiver.m_buffer->received_cap_count = installed;
}

Error Endpoint::wait_done(ThreadIpcContext& ctx, Message* const notification_into) noexcept {
  while (true) {
    sched::Scheduler::prepare_to_block();

    const IpcWait state = ctx.m_wait.load(std::memory_order_seq_cst);
    if (state == IpcWait::Done) {
      sched::Scheduler::cancel_block();
      break;
    }

    // A receiver with a bound notification also stops for that, unless a sender got to it.
    if (state == IpcWait::Receiving && notification_into != nullptr && bound_bits(ctx, false)) {
      sched::Scheduler::cancel_block();
      if (leave_for_notification(ctx)) {
        *notification_into = Message{};
        notification_into->tag = MsgTag::make(0, 0, 0, MsgFlags::IsAsyncNotification);
        notification_into->badge = bound_bits(ctx, true);
        ctx.m_wait.store(IpcWait::Idle, std::memory_order_relaxed);
        return Error::Success;
      }

      continue;
    }

    sched::Scheduler::schedule();
  }

  KASSERT(ctx.m_queued_on == nullptr);
  ctx.m_wait.store(IpcWait::Idle, std::memory_order_relaxed);
  return ctx.m_result;
}

bool Endpoint::leave_for_notification(ThreadIpcContext& ctx) noexcept {
  Guard guard{m_lock};
  if (ctx.m_wait.load(std::memory_order_relaxed) != IpcWait::Receiving) {
    return false;  // a sender took it off first, and the message is on its way
  }

  unlink_locked(ctx);
  return true;
}

std::expected<void, Error> Endpoint::send_impl(ThreadIpcContext& ctx, const Message& message,
                                               std::uint64_t badge, CapRights rights,
                                               bool wait) noexcept {
  KASSERT(ctx.m_thread == sched::current());
  if (!has_rights(rights, CapRights::Write)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  ThreadIpcContext* receiver = nullptr;
  {
    Guard guard{m_lock};
    if (m_closed) [[unlikely]] {
      return std::unexpected(Error::InvalidState);
    }

    if (m_head != nullptr && !m_senders_queued) {
      receiver = pop_locked();
      receiver->m_wait.store(IpcWait::Matched, std::memory_order_relaxed);
      m_delivered.fetch_add(1, std::memory_order_relaxed);
    } else if (!wait) {
      return std::unexpected(Error::WouldBlock);
    } else {
      ctx.m_outgoing = &message;
      ctx.m_badge = badge;
      ctx.m_rights = rights;
      ctx.m_is_call = false;
      ctx.m_result = Error::Success;
      ctx.m_wait.store(IpcWait::Sending, std::memory_order_seq_cst);
      m_senders_queued = true;
      push_locked(ctx);
    }
  }

  if (receiver == nullptr) {
    const Error result = wait_done(ctx, nullptr);
    return result == Error::Success ? std::expected<void, Error>{} : std::unexpected(result);
  }

  // The receiver waits, its buffer untouched, until finish() says it may look.
  deliver(ctx, message, badge, has_rights(rights, CapRights::Grant), false, *receiver,
          *receiver->m_incoming);
  finish(*receiver, Error::Success);
  return {};
}

std::expected<void, Error> Endpoint::send(ThreadIpcContext& ctx, const Message& message,
                                          std::uint64_t badge, CapRights rights) noexcept {
  return send_impl(ctx, message, badge, rights, true);
}

std::expected<void, Error> Endpoint::try_send(ThreadIpcContext& ctx, const Message& message,
                                              std::uint64_t badge, CapRights rights) noexcept {
  return send_impl(ctx, message, badge, rights, false);
}

std::expected<void, Error> Endpoint::call(ThreadIpcContext& ctx, const Message& request,
                                          Message& reply, std::uint64_t badge,
                                          CapRights rights) noexcept {
  KASSERT(ctx.m_thread == sched::current());
  if (!has_rights(rights, CapRights::Write)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  ctx.m_reply_buffer = &reply;
  ctx.m_reply_may_grant = has_rights(rights, CapRights::GrantReply);
  ctx.m_result = Error::Success;

  ThreadIpcContext* server = nullptr;
  {
    Guard guard{m_lock};
    if (m_closed) [[unlikely]] {
      return std::unexpected(Error::InvalidState);
    }

    if (m_head != nullptr && !m_senders_queued) {
      server = pop_locked();
      server->m_wait.store(IpcWait::Matched, std::memory_order_relaxed);
      m_delivered.fetch_add(1, std::memory_order_relaxed);

      // Before the server can see the call: it may answer the moment it is woken.
      ctx.m_wait.store(IpcWait::AwaitingReply, std::memory_order_seq_cst);
    } else {
      ctx.m_outgoing = &request;
      ctx.m_badge = badge;
      ctx.m_rights = rights;
      ctx.m_is_call = true;
      ctx.m_wait.store(IpcWait::Sending, std::memory_order_seq_cst);
      m_senders_queued = true;
      push_locked(ctx);
    }
  }

  if (server != nullptr) {
    deliver(ctx, request, badge, has_rights(rights, CapRights::Grant), true, *server,
            *server->m_incoming);
    KASSERT(server->m_reply_to == nullptr);  // recv() let go of any it had
    server->m_reply_to = &ctx;
    finish(*server, Error::Success);
  }

  const Error result = wait_done(ctx, nullptr);
  ctx.m_reply_buffer = nullptr;
  return result == Error::Success ? std::expected<void, Error>{} : std::unexpected(result);
}

std::expected<void, Error> Endpoint::recv_impl(ThreadIpcContext& ctx, Message& message,
                                               CapRights rights, bool wait) noexcept {
  KASSERT(ctx.m_thread == sched::current());
  if (!has_rights(rights, CapRights::Read)) [[unlikely]] {
    return std::unexpected(Error::AccessDenied);
  }

  // A call it got and did not answer will not be answered now.
  drop_caller(ctx);

  if (const std::uint64_t bits = bound_bits(ctx, true); bits != 0) {
    message = Message{};
    message.tag = MsgTag::make(0, 0, 0, MsgFlags::IsAsyncNotification);
    message.badge = bits;
    return {};
  }

  ThreadIpcContext* sender = nullptr;
  {
    Guard guard{m_lock};
    if (m_closed) [[unlikely]] {
      return std::unexpected(Error::InvalidState);
    }

    if (m_head != nullptr && m_senders_queued) {
      sender = pop_locked();
      sender->m_wait.store(IpcWait::Matched, std::memory_order_relaxed);
      m_delivered.fetch_add(1, std::memory_order_relaxed);
    } else if (!wait) {
      return std::unexpected(Error::WouldBlock);
    } else {
      ctx.m_incoming = &message;
      ctx.m_result = Error::Success;
      ctx.m_wait.store(IpcWait::Receiving, std::memory_order_seq_cst);
      m_senders_queued = false;
      push_locked(ctx);
    }
  }

  if (sender == nullptr) {
    const Error result = wait_done(ctx, &message);
    return result == Error::Success ? std::expected<void, Error>{} : std::unexpected(result);
  }

  deliver(*sender, *sender->m_outgoing, sender->m_badge,
          has_rights(sender->m_rights, CapRights::Grant), sender->m_is_call, ctx, message);

  if (sender->m_is_call) {
    // It goes on waiting, now for the answer, which is this thread's to give.
    sender->m_wait.store(IpcWait::AwaitingReply, std::memory_order_seq_cst);
    ctx.m_reply_to = sender;
  } else {
    finish(*sender, Error::Success);
  }

  return {};
}

std::expected<void, Error> Endpoint::recv(ThreadIpcContext& ctx, Message& message,
                                          CapRights rights) noexcept {
  return recv_impl(ctx, message, rights, true);
}

std::expected<void, Error> Endpoint::try_recv(ThreadIpcContext& ctx, Message& message,
                                              CapRights rights) noexcept {
  return recv_impl(ctx, message, rights, false);
}

std::expected<void, Error> Endpoint::reply(ThreadIpcContext& ctx, const Message& message) noexcept {
  KASSERT(ctx.m_thread == sched::current());
  ThreadIpcContext* caller = std::exchange(ctx.m_reply_to, nullptr);
  if (caller == nullptr) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  deliver(ctx, message, 0, caller->m_reply_may_grant, false, *caller, *caller->m_reply_buffer);
  finish(*caller, Error::Success);
  return {};
}

std::expected<void, Error> Endpoint::reply_recv(ThreadIpcContext& ctx, const Message& reply,
                                                Message& next, CapRights rights) noexcept {
  if (ctx.m_reply_to != nullptr) {
    (void)Endpoint::reply(ctx, reply);
  }

  return recv(ctx, next, rights);
}

std::expected<CapId, Error> Endpoint::save_reply(ThreadIpcContext& ctx) noexcept {
  KASSERT(ctx.m_thread == sched::current());
  if (ctx.m_reply_to == nullptr || ctx.cspace() == nullptr) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  // It counts against the saver, whose CSpace it sits in until it is used or deleted: a server
  // that saves replies and never gets round to them pays for that itself.
  const proc::Process* const process = ctx.m_thread->process();
  auto reply =
      Reply::create_charged(process != nullptr ? process->quota() : nullptr, *ctx.m_reply_to);
  if (!reply) [[unlikely]] {
    return std::unexpected(reply.error());
  }

  const auto id = ctx.cspace()->insert(**reply, CapRights::Write, CapFlags::OneShot);
  if (!id) [[unlikely]] {
    // Still this thread's to answer.
    (*reply)->detach();
    return std::unexpected(id.error());
  }

  ctx.m_reply_to = nullptr;
  return id;
}

std::expected<void, Error> Endpoint::reply_with(const ThreadIpcContext& ctx, CapId reply,
                                                const Message& message) noexcept {
  if (ctx.cspace() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  auto held = ctx.cspace()->consume<Reply>(reply, CapRights::Write);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->answer(ctx, message);
}

void Endpoint::on_unreachable() noexcept {
  ThreadIpcContext* waiters;
  {
    Guard guard{m_lock};
    m_closed = true;
    waiters = m_head;
    for (ThreadIpcContext* ctx = m_head; ctx != nullptr; ctx = ctx->m_next) {
      ctx->m_wait.store(IpcWait::Matched, std::memory_order_relaxed);
      ctx->m_queued_on = nullptr;
    }

    m_head = nullptr;
    m_tail = nullptr;
  }

  while (waiters != nullptr) {
    ThreadIpcContext* next = waiters->m_next;
    waiters->m_prev = nullptr;
    waiters->m_next = nullptr;
    finish(*waiters, Error::InvalidState);
    waiters = next;
  }
}

Reply::~Reply() noexcept {
  // Nobody answered, and nobody can now.
  if (m_caller != nullptr) {
    Endpoint::finish(*m_caller, Error::NotFound);
  }
}

void Reply::detach() noexcept {
  Guard guard{m_lock};
  m_caller = nullptr;
}

std::expected<void, Error> Reply::answer(const ThreadIpcContext& ctx,
                                         const Message& message) noexcept {
  ThreadIpcContext* caller;
  {
    Guard guard{m_lock};
    caller = std::exchange(m_caller, nullptr);
  }

  if (caller == nullptr) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  Endpoint::deliver(ctx, message, 0, caller->m_reply_may_grant, false, *caller,
                    *caller->m_reply_buffer);
  Endpoint::finish(*caller, Error::Success);
  return {};
}
}  // namespace kernel::core::ipc
