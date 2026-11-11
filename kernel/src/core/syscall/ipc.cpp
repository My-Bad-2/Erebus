#include <algorithm>
#include <cstddef>

#include "core/ipc/invoke.hpp"
#include "core/ipc/message.hpp"
#include "hal/user.hpp"
#include "handlers.hpp"

namespace kernel::core::syscall::handlers {
namespace {
using ipc::IpcBuffer;
using ipc::MsgTag;

[[nodiscard]] std::size_t overflow_words(const MsgTag tag) noexcept {
  const std::size_t length = std::min(tag.get_length(), MsgTag::MAX_TOTAL_WORDS);
  return length > MsgTag::MAX_INLINE_WORDS ? length - MsgTag::MAX_INLINE_WORDS : 0;
}

[[nodiscard]] std::size_t extra_caps(const MsgTag tag) noexcept {
  return std::min(tag.get_extra_caps(), MsgTag::MAX_EXTRA_CAPS);
}

[[nodiscard]] memory::VirtualAddress field(const memory::VirtualAddress buffer,
                                           const std::size_t offset) noexcept {
  return buffer + static_cast<memory::VirtualAddress::OffsetType>(offset);
}

// Copies the words and capability descriptors a message with @p tag carries from the user's
// buffer to the kernel's, for IPC to take from there.
// @return InvalidState if the thread named no buffer to take them from.
[[nodiscard]] std::expected<void, Error> stage(const ipc::ThreadIpcContext& ctx,
                                               const MsgTag tag) noexcept {
  const std::size_t words = overflow_words(tag);
  const std::size_t caps = extra_caps(tag);
  if (words == 0 && caps == 0) [[likely]] {
    return {};
  }

  IpcBuffer* const buffer = ctx.buffer();
  if (buffer == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  const memory::VirtualAddress user = ctx.user_buffer();
  return copy_from_user(field(user, offsetof(IpcBuffer, overflow_words)),
                        std::as_writable_bytes(std::span{buffer->overflow_words}.first(words)))
      .and_then([&] {
        return copy_from_user(field(user, offsetof(IpcBuffer, send_caps)),
                              std::as_writable_bytes(std::span{buffer->send_caps}.first(caps)));
      });
}

// The other way: what came with a received message goes from the kernel's buffer to the user's.
// IPC already cut the message down for a thread with no buffer. The message is the thread's by
// now, so a copy that fails cannot give it back; the tag says what happened, as it does for what
// went wrong on the way. Capabilities that came along are in the CSpace either way.
void publish(const ipc::ThreadIpcContext& ctx, ipc::Message& message) noexcept {
  const std::size_t words = overflow_words(message.tag);
  const std::size_t caps = extra_caps(message.tag);
  const IpcBuffer* const buffer = ctx.buffer();
  if ((words == 0 && caps == 0) || buffer == nullptr) [[likely]] {
    return;
  }

  const memory::VirtualAddress user = ctx.user_buffer();
  auto copied = copy_to_user(field(user, offsetof(IpcBuffer, overflow_words)),
                             std::as_bytes(std::span{buffer->overflow_words}.first(words)));
  if (copied && caps != 0) {
    copied = copy_to_user(field(user, offsetof(IpcBuffer, recv_caps)),
                          std::as_bytes(std::span{buffer->recv_caps}.first(caps)))
                 .and_then([&] {
                   return copy_to_user(field(user, offsetof(IpcBuffer, received_cap_count)),
                                       buffer->received_cap_count);
                 });
  }

  if (!copied) [[unlikely]] {
    message.tag.fail(copied.error());
  }
}

// What every message call starts with: the caller's context, and what it sends staged.
[[nodiscard]] std::expected<ipc::ThreadIpcContext*, Error> sender(const MsgTag tag) noexcept {
  return caller_context().and_then(
      [tag](ipc::ThreadIpcContext* ctx) -> std::expected<ipc::ThreadIpcContext*, Error> {
        return stage(*ctx, tag).transform([ctx] { return ctx; });
      });
}

// Hands a received message back to user code. @return What RAX gets for @p result.
std::int64_t deliver(Frame& frame, const ipc::ThreadIpcContext& ctx, ipc::Message& message,
                     const std::expected<void, Error>& result) noexcept {
  if (!result) [[unlikely]] {
    return encode_error(result.error());
  }

  publish(ctx, message);
  frame.set_message(message);
  return 0;
}

using SendFn = std::expected<void, Error> (*)(ipc::ThreadIpcContext&, ipc::CapId,
                                              const ipc::Message&) noexcept;
using RecvFn = std::expected<void, Error> (*)(ipc::ThreadIpcContext&, ipc::CapId,
                                              ipc::Message&) noexcept;

std::int64_t send_with(Frame& frame, const SendFn send) noexcept {
  const ipc::Message message = frame.message();
  const auto ctx = sender(message.tag);
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  return to_rax(send(**ctx, frame.cap<0>(), message));
}

std::int64_t recv_with(Frame& frame, const RecvFn recv) noexcept {
  const auto ctx = caller_context();
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  ipc::Message message{};
  return deliver(frame, **ctx, message, recv(**ctx, frame.cap<0>(), message));
}

using WaitFn = std::expected<std::uint64_t, Error> (*)(const ipc::ThreadIpcContext&,
                                                       ipc::CapId) noexcept;

std::int64_t wait_with(Frame& frame, const WaitFn wait) noexcept {
  const auto bits = caller_context().and_then(
      [&](const ipc::ThreadIpcContext* ctx) { return wait(*ctx, frame.cap<0>()); });
  if (!bits) [[unlikely]] {
    return encode_error(bits.error());
  }

  frame.set_second_result(*bits);
  return 0;
}
}  // namespace

std::int64_t ipc_call(Frame& frame) noexcept {
  const ipc::Message request = frame.message();
  const auto ctx = sender(request.tag);
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  ipc::Message reply{};
  return deliver(frame, **ctx, reply, ipc::call(**ctx, frame.cap<0>(), request, reply));
}

std::int64_t ipc_reply_recv(Frame& frame) noexcept {
  const ipc::Message reply = frame.message();
  const auto ctx = sender(reply.tag);
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  ipc::Message next{};
  return deliver(frame, **ctx, next, ipc::reply_recv(**ctx, frame.cap<0>(), reply, next));
}

std::int64_t ipc_send(Frame& frame) noexcept { return send_with(frame, &ipc::send); }

std::int64_t ipc_try_send(Frame& frame) noexcept { return send_with(frame, &ipc::try_send); }

std::int64_t ipc_recv(Frame& frame) noexcept { return recv_with(frame, &ipc::recv); }

std::int64_t ipc_try_recv(Frame& frame) noexcept { return recv_with(frame, &ipc::try_recv); }

std::int64_t ipc_reply(Frame& frame) noexcept {
  const ipc::Message message = frame.message();
  const auto ctx = sender(message.tag);
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  const ipc::CapId saved = frame.cap<0>();
  return to_rax(saved.is_null() ? ipc::reply(**ctx, message)
                                : ipc::reply_with(**ctx, saved, message));
}

std::int64_t ipc_save_reply(Frame&) noexcept {
  return to_rax(
      caller_context().and_then([](ipc::ThreadIpcContext* ctx) { return ipc::save_reply(*ctx); }));
}

std::int64_t ipc_signal(Frame& frame) noexcept {
  return to_rax(caller_context().and_then(
      [&](const ipc::ThreadIpcContext* ctx) { return ipc::signal(*ctx, frame.cap<0>()); }));
}

std::int64_t ipc_wait(Frame& frame) noexcept { return wait_with(frame, &ipc::wait); }

std::int64_t ipc_poll(Frame& frame) noexcept { return wait_with(frame, &ipc::poll); }

std::int64_t ipc_set_buffer(Frame& frame) noexcept {
  const auto ctx = caller_context();
  if (!ctx) [[unlikely]] {
    return encode_error(ctx.error());
  }

  const memory::VirtualAddress address = frame.address<0>();
  if (!address.is_null()) {
    if (!address.is_aligned(alignof(IpcBuffer))) [[unlikely]] {
      return encode_error(Error::InvalidAlignment);
    }

    if (!hw::user::is_user_range(address, sizeof(IpcBuffer))) [[unlikely]] {
      return encode_error(Error::InvalidArgument);
    }
  }

  // Not inside an IPC operation, being in this one.
  (*ctx)->set_user_buffer(address);
  return 0;
}
}  // namespace kernel::core::syscall::handlers
