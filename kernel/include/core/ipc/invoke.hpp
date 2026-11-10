/// @file invoke.hpp
/// @brief IPC by capability: what a thread does with the CapIds in its CSpace.
///
/// Each of these finds the capability in the context's CSpace, makes sure it names the right kind
/// of object and has the rights the operation takes, keeps a reference on the object for as long
/// as the operation lasts, and hands the capability's rights and badge on. They are what system
/// calls come down to, once there are any; kernel threads can use them as they are.
///
/// @pre The context belongs to the calling thread, and has a CSpace.
#pragma once

#include <cstdint>
#include <expected>

#include "core/cap.hpp"
#include "core/errors.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/message.hpp"

namespace kernel::core::ipc {
/// @brief Endpoint::send() through @p endpoint.
[[nodiscard]] std::expected<void, Error> send(ThreadIpcContext& ctx, CapId endpoint,
                                              const Message& message) noexcept;

/// @brief Endpoint::try_send() through @p endpoint.
[[nodiscard]] std::expected<void, Error> try_send(ThreadIpcContext& ctx, CapId endpoint,
                                                  const Message& message) noexcept;

/// @brief Endpoint::call() through @p endpoint.
[[nodiscard]] std::expected<void, Error> call(ThreadIpcContext& ctx, CapId endpoint,
                                              const Message& request, Message& reply) noexcept;

/// @brief Endpoint::recv() through @p endpoint.
[[nodiscard]] std::expected<void, Error> recv(ThreadIpcContext& ctx, CapId endpoint,
                                              Message& message) noexcept;

/// @brief Endpoint::try_recv() through @p endpoint.
[[nodiscard]] std::expected<void, Error> try_recv(ThreadIpcContext& ctx, CapId endpoint,
                                                  Message& message) noexcept;

/// @brief Endpoint::reply(): answers the last call received.
[[nodiscard]] std::expected<void, Error> reply(ThreadIpcContext& ctx,
                                               const Message& message) noexcept;

/// @brief Endpoint::reply_recv() through @p endpoint.
[[nodiscard]] std::expected<void, Error> reply_recv(ThreadIpcContext& ctx, CapId endpoint,
                                                    const Message& reply, Message& next) noexcept;

/// @brief Endpoint::save_reply(): the right to answer the last call, as a capability.
[[nodiscard]] std::expected<CapId, Error> save_reply(ThreadIpcContext& ctx) noexcept;

/// @brief Endpoint::reply_with(): answers through a saved Reply capability.
[[nodiscard]] std::expected<void, Error> reply_with(const ThreadIpcContext& ctx, CapId reply,
                                                    const Message& message) noexcept;

/// @brief Sets the capability's badge in the notification it names, or bit 0 if it has none.
///        Takes Write.
[[nodiscard]] std::expected<void, Error> signal(const ThreadIpcContext& ctx,
                                                CapId notification) noexcept;

/// @brief Notification::wait() through @p notification. Takes Read.
[[nodiscard]] std::expected<std::uint64_t, Error> wait(const ThreadIpcContext& ctx,
                                                       CapId notification) noexcept;

/// @brief Notification::poll() through @p notification. Takes Read.
[[nodiscard]] std::expected<std::uint64_t, Error> poll(const ThreadIpcContext& ctx,
                                                       CapId notification) noexcept;
}  // namespace kernel::core::ipc
