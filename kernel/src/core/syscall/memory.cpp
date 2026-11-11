#include <algorithm>

#include "core/ipc/driver_caps.hpp"
#include "core/ipc/object_cap.hpp"
#include "hal/user.hpp"
#include "handlers.hpp"
#include "memory/pmm.hpp"
#include "memory/quota.hpp"

namespace kernel::core::syscall::handlers {
namespace {
using memory::vmm::AccessFlags;

constexpr AccessFlags PROTECTION = AccessFlags::Read | AccessFlags::Write | AccessFlags::Execute;

[[nodiscard]] std::expected<ipc::Held<ipc::ProcessCap>, Error> other_process(
    const ipc::CapId id) noexcept {
  return caller_cspace().and_then(
      [id](const ipc::CSpace* cspace) { return cspace->acquire<ipc::ProcessCap>(id); });
}

[[nodiscard]] std::expected<ipc::Held<ipc::SharedVmObject>, Error> shared_object(
    const ipc::CapId id) noexcept {
  return caller_cspace().and_then(
      [id](const ipc::CSpace* cspace) { return cspace->acquire<ipc::SharedVmObject>(id); });
}
}  // namespace

std::int64_t mem_allocate(Frame& frame) noexcept {
  const auto size = size_arg(frame.arg<1>());
  const auto access =
      access_arg(frame.arg<2>(), PROTECTION | AccessFlags::Populate | AccessFlags::Stack);
  const auto page_size = page_size_arg(frame.arg<3>());
  const auto hint = hint_arg(frame.arg<4>());
  if (const Error error = first_error(size, access, page_size, hint); error != Error::Success)
      [[unlikely]] {
    return encode_error(error);
  }

  if (frame.cap<0>().is_null()) [[likely]] {
    return to_rax(caller_space().and_then([&](memory::vmm::AddressSpace* space) {
      return space->alloc(*size, *access, memory::vmm::CacheMode::WriteBack, *page_size,
                          memory::vmm::VMAType::Anonymous, 0, *hint);
    }));
  }

  return to_rax(other_process(frame.cap<0>()).and_then([&](const auto& process) {
    return process.object->allocate(process.rights, *size, *access, *page_size, *hint);
  }));
}

std::int64_t mem_protect(Frame& frame) noexcept {
  const memory::VirtualAddress address = frame.address<1>();
  const auto size = size_arg(frame.arg<2>());
  const auto access = access_arg(frame.arg<3>(), PROTECTION);
  if (const Error error = first_error(size, access); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  if (!hw::user::is_user_range(address, *size)) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  if (frame.cap<0>().is_null()) [[likely]] {
    return to_rax(caller_space().and_then(
        [&](memory::vmm::AddressSpace* space) { return space->protect(address, *size, *access); }));
  }

  return to_rax(other_process(frame.cap<0>()).and_then([&](const auto& process) {
    return process.object->protect(process.rights, address, *size, *access);
  }));
}

std::int64_t mem_unmap(Frame& frame) noexcept {
  const memory::VirtualAddress address = frame.address<1>();
  if (!hw::user::is_user_address(address)) [[unlikely]] {
    return encode_error(Error::InvalidArgument);
  }

  if (frame.cap<0>().is_null()) [[likely]] {
    return to_rax(caller_space().and_then(
        [&](memory::vmm::AddressSpace* space) { return space->free(address); }));
  }

  return to_rax(other_process(frame.cap<0>()).and_then([&](const auto& process) {
    return process.object->unmap(process.rights, address);
  }));
}

std::int64_t mem_map_object(Frame& frame) noexcept {
  const auto access = access_arg(frame.arg<2>(), PROTECTION | AccessFlags::Populate);
  const auto hint = hint_arg(frame.arg<5>());
  if (const Error error = first_error(access, hint); error != Error::Success) [[unlikely]] {
    return encode_error(error);
  }

  const ipc::CapId object = frame.cap<1>();
  const std::size_t first_page = frame.arg<3>();
  const std::size_t pages = frame.arg<4>();

  if (frame.cap<0>().is_null()) [[likely]] {
    const auto space = caller_space();
    if (!space) [[unlikely]] {
      return encode_error(space.error());
    }

    return to_rax(shared_object(object).and_then([&](const auto& held) {
      return held.object->map_into(**space, held.rights, *access, first_page, pages, *hint);
    }));
  }

  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  return to_rax(other_process(frame.cap<0>()).and_then([&](const auto& process) {
    return process.object->map_shared(process.rights, **cspace, object, *access, first_page, pages,
                                      *hint);
  }));
}

std::int64_t mem_derive_object(Frame& frame) noexcept {
  const auto rights = rights_arg(frame.arg<3>());
  if (!rights) [[unlikely]] {
    return encode_error(rights.error());
  }

  const auto cspace = caller_cspace();
  if (!cspace) [[unlikely]] {
    return encode_error(cspace.error());
  }

  const auto held = (*cspace)->acquire<ipc::SharedVmObject>(frame.cap<0>());
  if (!held) [[unlikely]] {
    return encode_error(held.error());
  }

  const auto slice =
      held->object->derive(held->rights, frame.arg<1>(), frame.arg<2>(), caller_quota());
  if (!slice) [[unlikely]] {
    return encode_error(slice.error());
  }

  // A part is a new object, so its capability is an original, with no more rights than the one
  // it was made from.
  return to_rax((*cspace)->insert(**slice, *rights & held->rights));
}

std::int64_t mem_resize_object(Frame& frame) noexcept {
  return to_rax(shared_object(frame.cap<0>()).and_then([&](const auto& held) {
    return held.object->resize(held.rights, frame.arg<1>());
  }));
}

std::int64_t mem_translate_object(Frame& frame) noexcept {
  return to_rax(shared_object(frame.cap<0>()).and_then([&](const auto& held) {
    return held.object->translate(held.rights, frame.arg<1>());
  }));
}

std::int64_t mem_info(const Frame& frame) noexcept {
  const memory::pmm::MemoryInfo machine = memory::pmm::memory_info();
  MemoryInfo info{
      .total_bytes = machine.total_bytes(),
      .free_bytes = machine.free_bytes(),
      .quota_used = 0,
      .quota_limit = 0,
      .quota_available = 0,
  };

  if (const memory::Quota* const quota = caller_quota()) {
    info.quota_used = quota->used();
    info.quota_limit = quota->limit();
    info.quota_available = ~std::uint64_t{0};
    for (const memory::Quota* level = quota; level != nullptr; level = level->parent()) {
      const std::size_t used = level->used();
      info.quota_available =
          std::min(info.quota_available, used < level->limit() ? level->limit() - used : 0);
    }
  }

  return to_rax(copy_to_user(frame.address<0>(), info));
}
}  // namespace kernel::core::syscall::handlers
