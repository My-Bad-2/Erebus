#include "common.hpp"

#include <algorithm>
#include <utility>

#include "core/ipc/object_cap.hpp"
#include "hal/user.hpp"
#include "proc/process.hpp"

namespace kernel::core::syscall {
std::expected<ipc::ThreadIpcContext*, Error> caller_context() noexcept {
  ipc::ThreadIpcContext* const ctx = ipc::ThreadCap::current_context();
  if (ctx == nullptr || ctx->cspace() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return ctx;
}

std::expected<ipc::CSpace*, Error> caller_cspace() noexcept {
  return caller_context().transform([](const ipc::ThreadIpcContext* ctx) { return ctx->cspace(); });
}

memory::Quota* caller_quota() noexcept {
  const proc::Process* const process = proc::current_process();
  return process != nullptr ? process->quota() : nullptr;
}

std::expected<memory::vmm::AddressSpace*, Error> caller_space() noexcept {
  const proc::Process* const process = proc::current_process();
  if (process == nullptr || process->address_space() == nullptr) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  return process->address_space();
}

std::expected<ipc::CapRef<ipc::CSpace>, Error> resolve_cspace(
    ipc::CSpace& own, const ipc::CapId id, const ipc::CapRights required) noexcept {
  if (id.is_null()) {
    return ipc::CapRef<ipc::CSpace>::share(&own);
  }

  return own.acquire<ipc::CSpace>(id, required).transform([](ipc::Held<ipc::CSpace>&& held) {
    return std::move(held.object);
  });
}

std::expected<void, Error> copy_from_user(const memory::VirtualAddress from,
                                          const std::span<std::byte> out) noexcept {
  if (!hw::user::is_user_range(from, out.size())) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  if (out.empty()) {
    return {};
  }

  return caller_space().and_then(
      [&](memory::vmm::AddressSpace* space) { return space->read_bytes(from, out); });
}

std::expected<void, Error> copy_to_user(const memory::VirtualAddress to,
                                        const std::span<const std::byte> bytes) noexcept {
  if (!hw::user::is_user_range(to, bytes.size())) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  if (bytes.empty()) {
    return {};
  }

  return caller_space().and_then(
      [&](memory::vmm::AddressSpace* space) { return space->write_bytes(to, bytes); });
}

std::expected<std::string_view, Error> copy_string_from_user(const memory::VirtualAddress from,
                                                             const std::size_t length,
                                                             const std::span<char> out) noexcept {
  const std::span<char> used = out.first(std::min(length, out.size()));
  return copy_from_user(from, std::as_writable_bytes(used)).transform([used] {
    return std::string_view{used.data(), used.size()};
  });
}

std::expected<memory::vmm::AccessFlags, Error> access_arg(
    const std::uint64_t raw, const memory::vmm::AccessFlags allowed) noexcept {
  if ((raw & ~static_cast<std::uint64_t>(std::to_underlying(allowed))) != 0) [[unlikely]] {
    return std::unexpected(Error::InvalidFlags);
  }

  return static_cast<memory::vmm::AccessFlags>(raw) | memory::vmm::AccessFlags::User;
}

std::expected<memory::vmm::PageSize, Error> page_size_arg(const std::uint64_t raw) noexcept {
  using memory::vmm::PageSize;
  switch (raw) {
    case 0:
    case std::to_underlying(PageSize::Size4K):
      return PageSize::Size4K;
    case std::to_underlying(PageSize::Size2M):
      return PageSize::Size2M;
    case std::to_underlying(PageSize::Size1G):
      return PageSize::Size1G;
    default:
      return std::unexpected(Error::InvalidArgument);
  }
}

std::expected<memory::vmm::CacheMode, Error> cache_mode_arg(const std::uint64_t raw) noexcept {
  if (raw > std::to_underlying(memory::vmm::CacheMode::WriteProtected)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  return static_cast<memory::vmm::CacheMode>(raw);
}

std::expected<ipc::CapRights, Error> rights_arg(const std::uint64_t raw) noexcept {
  if (raw > std::to_underlying(ipc::CapRights::All)) [[unlikely]] {
    return std::unexpected(Error::InvalidFlags);
  }

  return static_cast<ipc::CapRights>(raw);
}

std::expected<memory::VirtualAddress, Error> hint_arg(const std::uint64_t raw) noexcept {
  const memory::VirtualAddress hint{raw};
  if (!hint.is_null() && !hw::user::is_user_address(hint)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  return hint;
}

std::expected<std::size_t, Error> size_arg(const std::uint64_t raw) noexcept {
  if (raw > hw::user::user_space_end()) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  return raw;
}
}  // namespace kernel::core::syscall
