#include <algorithm>
#include <array>

#include "core/ipc/driver_caps.hpp"
#include "core/ipc/notification.hpp"
#include "core/ipc/object_cap.hpp"
#include "hal/user.hpp"
#include "handlers.hpp"
#include "utils/maths.hpp"

namespace kernel::core::syscall::handlers {
namespace {
constexpr std::size_t MAX_DMA_SEGMENTS = 32;

template <typename T>
[[nodiscard]] std::expected<ipc::Held<T>, Error> held(
    const ipc::CapId id, const ipc::CapRights required = ipc::CapRights::None) noexcept {
  return caller_cspace().and_then(
      [&](const ipc::CSpace* cspace) { return cspace->template acquire<T>(id, required); });
}
}  // namespace

std::int64_t dev_irq_bind(Frame& frame) noexcept {
  const auto irq = held<ipc::IrqHandler>(frame.cap<0>());
  if (!irq) [[unlikely]] {
    return encode_error(irq.error());
  }

  const ipc::CapId notification_id = frame.cap<1>();
  if (notification_id.is_null()) {
    return to_rax(irq->object->clear_notification(irq->rights));
  }

  const auto notification = held<ipc::Notification>(notification_id, ipc::CapRights::Write);
  if (!notification) [[unlikely]] {
    return encode_error(notification.error());
  }

  // The badge it was asked for, or the one its capability carries, as a signal through it would
  // raise, or bit 0 for one without.
  std::uint64_t badge = frame.arg<2>();
  if (badge == 0) {
    badge = notification->badge != 0 ? notification->badge : 1;
  }

  return to_rax(irq->object->set_notification(irq->rights, *notification->object, badge));
}

std::int64_t dev_irq_ack(Frame& frame) noexcept {
  return to_rax(held<ipc::IrqHandler>(frame.cap<0>()).and_then([](const auto& irq) {
    return irq.object->acknowledge(irq.rights);
  }));
}

std::int64_t dev_mmio_map(Frame& frame) noexcept {
  const auto hint = hint_arg(frame.arg<4>());
  if (!hint) [[unlikely]] {
    return encode_error(hint.error());
  }

  const ipc::CapId region = frame.cap<1>();
  const std::size_t offset = frame.arg<2>();
  const std::size_t length = frame.arg<3>();

  if (frame.cap<0>().is_null()) [[likely]] {
    const auto space = caller_space();
    if (!space) [[unlikely]] {
      return encode_error(space.error());
    }

    return to_rax(held<ipc::MmioRegion>(region).and_then([&](const auto& mmio) {
      return mmio.object->map_into(**space, mmio.rights, offset, length, true, *hint);
    }));
  }

  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  return to_rax(
      (*cspace)->acquire<ipc::ProcessCap>(frame.cap<0>()).and_then([&](const auto& process) {
        return process.object->map_mmio(process.rights, **cspace, region, offset, length, *hint);
      }));
}

std::int64_t dev_mmio_derive(Frame& frame) noexcept {
  const auto cache = cache_mode_arg(frame.arg<3>());
  const auto rights = rights_arg(frame.arg<4>());
  if (const Error error = first_error(cache, rights); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  const auto mmio = (*cspace)->acquire<ipc::MmioRegion>(frame.cap<0>());
  if (!mmio) [[unlikely]] {
    return encode_error(mmio.error());
  }

  const auto part =
      mmio->object->derive(mmio->rights, frame.arg<1>(), frame.arg<2>(), *cache, caller_quota());
  if (!part) [[unlikely]] {
    return encode_error(part.error());
  }

  return to_rax((*cspace)->insert(**part, *rights & mmio->rights));
}

std::int64_t dev_dma_alloc(Frame& frame) noexcept {
  const auto size = size_arg(frame.arg<1>());
  const auto cache = cache_mode_arg(frame.arg<2>());
  if (const Error error = first_error(size, cache); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  // Where the segments go is checked before anything is allocated for them.
  const memory::VirtualAddress segments_at = frame.address<3>();
  const std::size_t wanted = segments_at.is_null() ? 0 : std::min(frame.arg<4>(), MAX_DMA_SEGMENTS);
  if (!hw::user::is_user_range(segments_at, wanted * sizeof(ipc::DmaSegment))) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  const auto space = caller_space();
  const auto pool = held<ipc::DmaPool>(frame.cap<0>());
  if (const Error error = first_error(space, pool); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  std::array<ipc::DmaSegment, MAX_DMA_SEGMENTS> segments{};
  const std::span<ipc::DmaSegment> wanted_segments = std::span{segments}.first(wanted);
  const auto buffer =
      pool->object->allocate(**space, pool->rights, *size, true, *cache, wanted_segments);
  if (!buffer) [[unlikely]] {
    return encode_error(buffer.error());
  }

  const std::size_t pages = utils::maths::div_round_up(buffer->size_bytes, memory::PAGE_SIZE);
  const auto copied =
      copy_to_user(segments_at, std::as_bytes(wanted_segments.first(std::min(pages, wanted))));
  if (!copied) [[unlikely]] {
    // The address was checked, so the memory there went away meanwhile. Nobody would know where
    // the buffer is, so it goes too.
    (void)pool->object->free(**space, pool->rights, buffer->virt_addr);
    return encode_error(copied.error());
  }

  frame.set_second_result(buffer->first_phys_addr.value());
  return to_rax(std::expected<memory::VirtualAddress, Error>{buffer->virt_addr});
}

std::int64_t dev_dma_free(Frame& frame) noexcept {
  const auto space = caller_space();
  const auto pool = held<ipc::DmaPool>(frame.cap<0>());
  if (const Error error = first_error(space, pool); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  return to_rax(pool->object->free(**space, pool->rights, frame.address<1>()));
}
}  // namespace kernel::core::syscall::handlers
