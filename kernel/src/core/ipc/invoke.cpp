#include "core/ipc/invoke.hpp"

#include "core/cspace.hpp"
#include "core/ipc/notification.hpp"

namespace kernel::core::ipc {
namespace {
template <typename T>
[[nodiscard]] std::expected<Held<T>, Error> find(const ThreadIpcContext& ctx, const CapId id,
                                                 const CapRights required) noexcept {
  if (ctx.cspace() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return ctx.cspace()->acquire<T>(id, required);
}
}  // namespace

std::expected<void, Error> send(ThreadIpcContext& ctx, CapId endpoint,
                                const Message& message) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Write);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->send(ctx, message, held->badge, held->rights);
}

std::expected<void, Error> try_send(ThreadIpcContext& ctx, CapId endpoint,
                                    const Message& message) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Write);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->try_send(ctx, message, held->badge, held->rights);
}

std::expected<void, Error> call(ThreadIpcContext& ctx, CapId endpoint, const Message& request,
                                Message& reply) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Write);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->call(ctx, request, reply, held->badge, held->rights);
}

std::expected<void, Error> recv(ThreadIpcContext& ctx, CapId endpoint, Message& message) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Read);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->recv(ctx, message, held->rights);
}

std::expected<void, Error> try_recv(ThreadIpcContext& ctx, CapId endpoint,
                                    Message& message) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Read);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->try_recv(ctx, message, held->rights);
}

std::expected<void, Error> reply(ThreadIpcContext& ctx, const Message& message) noexcept {
  return Endpoint::reply(ctx, message);
}

std::expected<void, Error> reply_recv(ThreadIpcContext& ctx, CapId endpoint, const Message& reply,
                                      Message& next) noexcept {
  const auto held = find<Endpoint>(ctx, endpoint, CapRights::Read);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->reply_recv(ctx, reply, next, held->rights);
}

std::expected<CapId, Error> save_reply(ThreadIpcContext& ctx) noexcept {
  return Endpoint::save_reply(ctx);
}

std::expected<void, Error> reply_with(const ThreadIpcContext& ctx, CapId reply,
                                      const Message& message) noexcept {
  return Endpoint::reply_with(ctx, reply, message);
}

std::expected<void, Error> signal(const ThreadIpcContext& ctx, CapId notification) noexcept {
  const auto held = find<Notification>(ctx, notification, CapRights::Write);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  held->object->signal(held->badge != 0 ? held->badge : 1);
  return {};
}

std::expected<std::uint64_t, Error> wait(const ThreadIpcContext& ctx, CapId notification) noexcept {
  const auto held = find<Notification>(ctx, notification, CapRights::Read);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->wait();
}

std::expected<std::uint64_t, Error> poll(const ThreadIpcContext& ctx, CapId notification) noexcept {
  const auto held = find<Notification>(ctx, notification, CapRights::Read);
  if (!held) [[unlikely]] {
    return std::unexpected(held.error());
  }

  return held->object->poll();
}
}  // namespace kernel::core::ipc
