/// @file endpoint.hpp
/// @brief Synchronous IPC: endpoints, the threads that meet on them, and replies.
///
/// An endpoint is a place to meet. A sender and a receiver that come to it meet there: whichever
/// comes first waits in the endpoint's queue, the second takes it off, moves the message across,
/// and wakes it. Nothing is buffered, so a message is never copied twice, and a sender knows its
/// message is through when send() comes back.
///
/// call() is a send that waits for an answer. The receiver gets the right to answer along with the
/// message, and either answers with reply() before it receives again, or keeps the right for later
/// as a Reply capability (save_reply()), which it can hand to someone else to answer with. An
/// answer that will not come any more, because the receiver went on without it or the capability
/// went, ends the call with Error::NotFound.
///
/// ### Waiting
///
/// A thread that waits blocks the scheduler's way (see sched/scheduler.hpp), on the state of its
/// ThreadIpcContext: queued, taken off by a partner who is busy with the message, waiting for a
/// reply, or done. The partner does the work and wakes it through a sched::WakeList, so the waiter
/// may be destroyed the moment it sees itself done, and the waker does not care.
///
/// ### Closing
///
/// When the last capability to an endpoint goes, nobody can come to it any more, so whoever waits
/// in it never would get anywhere. They are woken with Error::InvalidState, and so is every later
/// operation by someone still holding a reference.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>

#include "core/cap.hpp"
#include "core/cspace.hpp"
#include "core/errors.hpp"
#include "core/ipc/message.hpp"
#include "sched/thread.hpp"
#include "utils/lock.hpp"

namespace kernel::core::ipc {
class Endpoint;
class Notification;
class Reply;

/// @brief Where a thread is in an IPC operation.
enum class IpcWait : std::uint8_t {
  Idle,           ///< Not in one.
  Sending,        ///< Queued on an endpoint, with a message to give.
  Receiving,      ///< Queued on an endpoint, waiting for one.
  Matched,        ///< Taken off the queue by a partner, which is moving the message.
  AwaitingReply,  ///< Its call is through, and the answer has not come yet.
  Done,           ///< Over. The result says how.
};

/// @brief A thread's side of IPC: its CSpace and buffer, and what it is waiting for.
///
/// Used by its thread only, which has to outlive it. ThreadCap has one for the thread it names; a
/// kernel thread can keep one on its stack. Destroying one fails a call it was to answer, and
/// unbinds its notification.
class ThreadIpcContext {
 public:
  /// @brief A context for the calling thread.
  explicit ThreadIpcContext(CSpace* cspace = nullptr, IpcBuffer* buffer = nullptr) noexcept;

  /// @brief A context for @p thread, which is the only one that may use it.
  ThreadIpcContext(sched::Thread* thread, CSpace* cspace, IpcBuffer* buffer) noexcept;

  ~ThreadIpcContext() noexcept;

  ThreadIpcContext(const ThreadIpcContext&) = delete;
  ThreadIpcContext& operator=(const ThreadIpcContext&) = delete;

  [[nodiscard]] sched::Thread* thread() const noexcept { return m_thread; }
  [[nodiscard]] CSpace* cspace() const noexcept { return m_cspace.get(); }
  [[nodiscard]] IpcBuffer* buffer() const noexcept { return m_buffer; }
  /// @brief The bound notification. Only good in an RCU read section, unless the caller is the
  ///        one who binds and unbinds it.
  [[nodiscard]] Notification* bound_notification() const noexcept {
    return m_bound.load(std::memory_order_acquire);
  }

  /// @brief Whether a call is waiting for this thread to answer it.
  [[nodiscard]] bool has_caller() const noexcept { return m_reply_to != nullptr; }

  /// @brief Changes the CSpace, which the context holds a reference to. Only by its thread, or
  ///        before it runs.
  void set_cspace(CSpace* cspace) noexcept;

  /// @brief The thread is gone: fails the call it was to answer, and unbinds its notification.
  void abandon() noexcept;
  void set_buffer(IpcBuffer* buffer) noexcept { m_buffer = buffer; }

 private:
  friend class Endpoint;
  friend class Notification;
  friend class Reply;

  sched::Thread* m_thread;
  CapRef<CSpace> m_cspace;
  IpcBuffer* m_buffer;

  std::atomic<IpcWait> m_wait{IpcWait::Idle};
  Error m_result{Error::Success};

  // While queued on an endpoint, under its lock.
  Endpoint* m_queued_on{nullptr};
  ThreadIpcContext* m_prev{nullptr};
  ThreadIpcContext* m_next{nullptr};
  const Message* m_outgoing{nullptr};   ///< A sender's message.
  Message* m_incoming{nullptr};         ///< A receiver's place for one.
  std::uint64_t m_badge{0};             ///< A sender's badge, from its capability.
  CapRights m_rights{CapRights::None};  ///< A sender's rights on the endpoint.
  bool m_is_call{false};

  // A caller's side of a call.
  Message* m_reply_buffer{nullptr};
  bool m_reply_may_grant{false};  ///< Its capability had GrantReply.

  // A receiver's side: the call it got and has not answered yet. Its own, but for the reply that
  // fails it if the context goes.
  ThreadIpcContext* m_reply_to{nullptr};

  // With a reference. Set and cleared under the notification's lock; read in read sections, as
  // the notification only goes a grace period after it is unbound.
  std::atomic<Notification*> m_bound{nullptr};
};

/// @brief A synchronous IPC endpoint. See the file comment.
///
/// The operations take the context of the calling thread, and the rights and badge of the
/// capability it used; core/ipc/invoke.hpp does that part. The caller holds a reference to the
/// endpoint for as long as an operation lasts.
class alignas(std::hardware_destructive_interference_size) Endpoint final
    : public TypedCapObject<Endpoint, CapType::Endpoint> {
 public:
  constexpr Endpoint() noexcept = default;
  ~Endpoint() noexcept;

  /// @brief Sends @p message, waiting for a receiver if there is none. Takes Write, and Grant to
  ///        send capabilities along.
  [[nodiscard]] std::expected<void, Error> send(ThreadIpcContext& ctx, const Message& message,
                                                std::uint64_t badge, CapRights rights) noexcept;

  /// @brief send() that does not wait. @return WouldBlock if no receiver is there.
  [[nodiscard]] std::expected<void, Error> try_send(ThreadIpcContext& ctx, const Message& message,
                                                    std::uint64_t badge, CapRights rights) noexcept;

  /// @brief Sends @p request and waits for the answer, in @p reply. Takes Write; GrantReply lets
  ///        the answer bring capabilities.
  /// @return NotFound if the answer will not come; InvalidState if the endpoint closed first.
  [[nodiscard]] std::expected<void, Error> call(ThreadIpcContext& ctx, const Message& request,
                                                Message& reply, std::uint64_t badge,
                                                CapRights rights) noexcept;

  /// @brief Waits for a message, into @p message. Takes Read. A bound notification with bits
  ///        pending is delivered instead, as a message with MsgFlags::IsAsyncNotification.
  ///
  /// A call that was received before and not answered is failed first.
  [[nodiscard]] std::expected<void, Error> recv(ThreadIpcContext& ctx, Message& message,
                                                CapRights rights) noexcept;

  /// @brief recv() that does not wait. @return WouldBlock if no sender is there.
  [[nodiscard]] std::expected<void, Error> try_recv(ThreadIpcContext& ctx, Message& message,
                                                    CapRights rights) noexcept;

  /// @brief Answers the call the calling thread received last.
  /// @return NotFound if there is none.
  [[nodiscard]] static std::expected<void, Error> reply(ThreadIpcContext& ctx,
                                                        const Message& message) noexcept;

  /// @brief reply() if there is a call to answer, then recv().
  [[nodiscard]] std::expected<void, Error> reply_recv(ThreadIpcContext& ctx, const Message& reply,
                                                      Message& next, CapRights rights) noexcept;

  /// @brief Turns the right to answer the last call into a one-shot Reply capability, in the
  ///        context's CSpace, so that the thread can receive again and answer later.
  [[nodiscard]] static std::expected<CapId, Error> save_reply(ThreadIpcContext& ctx) noexcept;

  /// @brief Answers through the Reply capability @p reply, which goes in the process.
  [[nodiscard]] static std::expected<void, Error> reply_with(const ThreadIpcContext& ctx,
                                                             CapId reply,
                                                             const Message& message) noexcept;

  /// @brief Messages that went through so far.
  [[nodiscard]] std::uint64_t delivered() const noexcept {
    return m_delivered.load(std::memory_order_relaxed);
  }

  /// @brief The last capability went: whoever waits here is woken, and nobody waits from now on.
  void on_unreachable() noexcept;

 private:
  friend class Notification;
  friend class Reply;
  friend class ThreadIpcContext;

  [[nodiscard]] std::expected<void, Error> send_impl(ThreadIpcContext& ctx, const Message& message,
                                                     std::uint64_t badge, CapRights rights,
                                                     bool wait) noexcept;
  [[nodiscard]] std::expected<void, Error> recv_impl(ThreadIpcContext& ctx, Message& message,
                                                     CapRights rights, bool wait) noexcept;

  void push_locked(ThreadIpcContext& ctx) noexcept;
  [[nodiscard]] ThreadIpcContext* pop_locked() noexcept;
  void unlink_locked(ThreadIpcContext& ctx) noexcept;

  // Takes a receiver out of the queue for a bound notification, if no sender got to it first.
  [[nodiscard]] bool leave_for_notification(ThreadIpcContext& ctx) noexcept;

  // Waits for the context's operation to be over. @return Its result.
  [[nodiscard]] Error wait_done(ThreadIpcContext& ctx, Message* notification_into) noexcept;

  // Moves a message, with its capabilities, from @p sender to @p receiver.
  static void deliver(const ThreadIpcContext& sender, const Message& message, std::uint64_t badge,
                      bool may_grant, bool is_call, const ThreadIpcContext& receiver,
                      Message& into) noexcept;

  // Ends @p ctx's wait with @p result and wakes its thread.
  static void finish(ThreadIpcContext& ctx, Error result) noexcept;

  // Fails the call @p ctx received and did not answer.
  static void drop_caller(ThreadIpcContext& ctx) noexcept;

  utils::QSpinlock m_lock{};
  ThreadIpcContext* m_head{nullptr};
  ThreadIpcContext* m_tail{nullptr};
  bool m_senders_queued{false};  ///< What the queue holds, when it holds anything.
  bool m_closed{false};
  std::atomic<std::uint64_t> m_delivered{0};
};

/// @brief The right to answer one call, saved for later. See Endpoint::save_reply().
///
/// Holds the waiting caller. Going away without an answer fails the call, so a server that drops
/// it, or dies with it, does not leave the caller waiting forever.
class alignas(16) Reply final : public TypedCapObject<Reply, CapType::Reply> {
 public:
  explicit Reply(ThreadIpcContext& caller) noexcept : m_caller(&caller) {}
  ~Reply() noexcept;

  /// @brief Answers the call with @p message, from @p ctx's thread. Only once.
  /// @return NotFound if it was answered already.
  [[nodiscard]] std::expected<void, Error> answer(const ThreadIpcContext& ctx,
                                                  const Message& message) noexcept;

  /// @brief Lets go of the caller without failing its call, which someone else answers.
  void detach() noexcept;

 private:
  utils::QSpinlock m_lock{};
  ThreadIpcContext* m_caller;
};
}  // namespace kernel::core::ipc
