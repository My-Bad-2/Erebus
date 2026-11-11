#include <utility>

#include "core/ipc/driver_caps.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/notification.hpp"
#include "handlers.hpp"

namespace kernel::core::syscall::handlers {
namespace {
// Puts the original capability to a new object in @p cspace, which keeps the object alive from
// then on, with every right.
template <typename T>
[[nodiscard]] std::int64_t insert_new(ipc::CSpace& cspace,
                                      const std::expected<ipc::CapRef<T>, Error>& made) noexcept {
  if (!made) [[unlikely]] {
    return encode_error(made.error());
  }

  return to_rax(cspace.insert(**made, ipc::CapRights::All));
}

// The two CSpaces a call moves a capability between: arguments 0 and 2.
struct Pair {
  ipc::CapRef<ipc::CSpace> source;
  ipc::CapRef<ipc::CSpace> target;
};

[[nodiscard]] std::expected<Pair, Error> resolve_pair(const Frame& frame,
                                                      const ipc::CapRights from_source) noexcept {
  const auto own = caller_cspace();
  if (!own) [[unlikely]] {
    return std::unexpected(own.error());
  }

  auto source = resolve_cspace(**own, frame.cap<0>(), from_source);
  if (!source) [[unlikely]] {
    return std::unexpected(source.error());
  }

  auto target = resolve_cspace(**own, frame.cap<2>(), ipc::CapRights::Write);
  if (!target) [[unlikely]] {
    return std::unexpected(target.error());
  }

  return Pair{.source = std::move(*source), .target = std::move(*target)};
}

[[nodiscard]] std::expected<ipc::CapRef<ipc::CSpace>, Error> resolve_one(
    const Frame& frame, const ipc::CapRights required) noexcept {
  return caller_cspace().and_then(
      [&](ipc::CSpace* own) { return resolve_cspace(*own, frame.cap<0>(), required); });
}
}  // namespace

std::int64_t cap_create(Frame& frame) noexcept {
  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  if (frame.arg<0>() > 0xFF) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  memory::Quota* const quota = caller_quota();
  switch (static_cast<ipc::CapType>(frame.arg<0>())) {
    case ipc::CapType::Endpoint:
      return insert_new(**cspace, ipc::Endpoint::create_charged(quota));

    case ipc::CapType::Notification:
      return insert_new(**cspace, ipc::Notification::create_charged(quota));

    case ipc::CapType::CSpace:
      return insert_new(**cspace, ipc::CSpace::create_charged(quota));

    case ipc::CapType::VmObject: {
      const auto size = size_arg(frame.arg<1>());
      const auto page_size = page_size_arg(frame.arg<2>());
      if (const Error error = first_error(size, page_size); error != Error::Success) [[unlikely]] {
        return encode_error(error);
      }

      return insert_new(**cspace, ipc::SharedVmObject::create(*size, *page_size, quota));
    }

    default:
      // Interrupts, MMIO, DMA pools, threads and processes stand for something nobody can make
      // out of nothing: their capabilities are handed down from whoever has them.
      return encode_error(Error::InvalidArgument);
  }
}

std::int64_t cap_mint(Frame& frame) noexcept {
  const auto rights = rights_arg(frame.arg<3>());
  if (!rights) [[unlikely]] {
    return encode_error(rights.error());
  }

  if (frame.arg<5>() > 0xFF) [[unlikely]] {
    return encode_error(Error::InvalidFlags);
  }

  const auto pair = resolve_pair(frame, ipc::CapRights::Read);
  if (!pair) [[unlikely]] {
    return encode_error(pair.error());
  }

  return to_rax(pair->source->mint(frame.cap<1>(), *pair->target, *rights, frame.arg<4>(),
                                   static_cast<ipc::CapFlags>(frame.arg<5>())));
}

std::int64_t cap_copy(Frame& frame) noexcept {
  const auto mask = rights_arg(frame.arg<3>());
  if (!mask) [[unlikely]] {
    return encode_error(mask.error());
  }

  const auto pair = resolve_pair(frame, ipc::CapRights::Read);
  if (!pair) [[unlikely]] {
    return encode_error(pair.error());
  }

  return to_rax(pair->source->copy(frame.cap<1>(), *pair->target, *mask));
}

std::int64_t cap_move(Frame& frame) noexcept {
  const auto pair = resolve_pair(frame, ipc::CapRights::Write);
  if (!pair) [[unlikely]] {
    return encode_error(pair.error());
  }

  return to_rax(pair->source->move(frame.cap<1>(), *pair->target));
}

std::int64_t cap_delete(const Frame& frame) noexcept {
  return to_rax(resolve_one(frame, ipc::CapRights::Write).and_then([&](const auto& cspace) {
    return cspace->remove(frame.cap<1>());
  }));
}

std::int64_t cap_revoke(const Frame& frame) noexcept {
  return to_rax(resolve_one(frame, ipc::CapRights::Revoke).and_then([&](const auto& cspace) {
    return cspace->revoke(frame.cap<1>());
  }));
}
}  // namespace kernel::core::syscall::handlers
