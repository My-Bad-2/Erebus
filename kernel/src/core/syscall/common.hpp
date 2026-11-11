#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

#include "core/cap.hpp"
#include "core/cspace.hpp"
#include "core/errors.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/message.hpp"
#include "core/syscall/abi.hpp"
#include "core/syscall/syscall.hpp"
#include "hal/interrupts/interrupts.hpp"
#include "memory/address.hpp"
#include "memory/quota.hpp"
#include "memory/vmm/pagemap/flags.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::core::syscall {
class Frame {
  using Registers = hw::interrupts::RegisterContext;

  static constexpr std::uint64_t Registers::* ARGUMENTS[] = {
      &Registers::rdi, &Registers::rsi, &Registers::rdx,
      &Registers::r10, &Registers::r8,  &Registers::r9,
  };

 public:
  Frame(Registers& regs, const UserReturn& from) noexcept : m_regs(regs), m_from(from) {}

  [[nodiscard]] std::uint64_t number() const noexcept { return m_regs.rax; }
  [[nodiscard]] const Registers& registers() const noexcept { return m_regs; }
  [[nodiscard]] const UserReturn& from() const noexcept { return m_from; }

  template <std::size_t N>
  [[nodiscard]] std::uint64_t arg() const noexcept {
    static_assert(N < std::size(ARGUMENTS));
    return m_regs.*ARGUMENTS[N];
  }

  template <std::size_t N>
  [[nodiscard]] ipc::CapId cap() const noexcept {
    return ipc::CapId{static_cast<std::uint32_t>(arg<N>())};
  }

  template <std::size_t N>
  [[nodiscard]] memory::VirtualAddress address() const noexcept {
    return memory::VirtualAddress{arg<N>()};
  }

  [[nodiscard]] ipc::Message message() const noexcept {
    ipc::Message message;
    message.tag = ipc::MsgTag{m_regs.rsi};
    message.regs = {m_regs.rdx, m_regs.r10, m_regs.r8, m_regs.r9, m_regs.r12, m_regs.r13};
    return message;
  }

  void set_message(const ipc::Message& message) const noexcept {
    m_regs.rdi = message.badge;
    m_regs.rsi = message.tag.raw();
    m_regs.rdx = message.regs[0];
    m_regs.r10 = message.regs[1];
    m_regs.r8 = message.regs[2];
    m_regs.r9 = message.regs[3];
    m_regs.r12 = message.regs[4];
    m_regs.r13 = message.regs[5];
  }

  void set_second_result(const std::uint64_t value) const noexcept { m_regs.rdi = value; }
  void set_result(const std::int64_t rax) const noexcept {
    m_regs.rax = static_cast<std::uint64_t>(rax);
  }

 private:
  Registers& m_regs;
  const UserReturn& m_from;
};

using Handler = std::int64_t (*)(Frame& frame) noexcept;

template <typename T>
[[nodiscard]] std::int64_t to_rax(const std::expected<T, Error>& result) noexcept {
  if (!result) [[unlikely]] {
    return encode_error(result.error());
  }

  if constexpr (std::is_void_v<T>) {
    return 0;
  } else if constexpr (std::is_same_v<T, ipc::CapId>) {
    return result->raw();
  } else if constexpr (std::is_same_v<T, memory::VirtualAddress> ||
                       std::is_same_v<T, memory::PhysicalAddress>) {
    return static_cast<std::int64_t>(result->value());
  } else {
    static_assert(std::is_integral_v<T>);
    return static_cast<std::int64_t>(*result);
  }
}

/// The error of the first of @p results that failed, in order, or Error::Success: for calls that
/// look at all their arguments before they do anything.
template <typename... Results>
[[nodiscard]] constexpr Error first_error(const Results&... results) noexcept {
  Error error = Error::Success;
  ((error = (error == Error::Success && !results.has_value()) ? results.error() : error), ...);
  return error;
}

/// The calling thread's IPC context, which every call that names a capability needs, as that is
/// where the thread's CSpace is.
/// @return InvalidState for a thread without one, or without a CSpace: it has nothing to name.
[[nodiscard]] std::expected<ipc::ThreadIpcContext*, Error> caller_context() noexcept;

/// The calling thread's CSpace, as caller_context() finds it.
[[nodiscard]] std::expected<ipc::CSpace*, Error> caller_cspace() noexcept;

/// What objects made for the calling process count against: its tree's quota. Every object a
/// call makes on its behalf is charged to this, so that no process can have the kernel hold more
/// for it than that.
[[nodiscard]] memory::Quota* caller_quota() noexcept;

/// The address space of the calling process. It stays for as long as the calling thread does.
[[nodiscard]] std::expected<memory::vmm::AddressSpace*, Error> caller_space() noexcept;

/// The CSpace @p id names in @p own, which must have @p required on it, or @p own itself for 0.
[[nodiscard]] std::expected<ipc::CapRef<ipc::CSpace>, Error> resolve_cspace(
    ipc::CSpace& own, ipc::CapId id, ipc::CapRights required) noexcept;

/// Fills @p out from user memory at @p from, in the calling process.
/// @return InvalidArgument for a range that is not all user memory, or whatever the address space
///         says about a page that is not there to read.
[[nodiscard]] std::expected<void, Error> copy_from_user(memory::VirtualAddress from,
                                                        std::span<std::byte> out) noexcept;

/// Copies @p bytes to user memory at @p to, in the calling process.
[[nodiscard]] std::expected<void, Error> copy_to_user(memory::VirtualAddress to,
                                                      std::span<const std::byte> bytes) noexcept;

template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] std::expected<void, Error> copy_to_user(const memory::VirtualAddress to,
                                                      const T& value) noexcept {
  return copy_to_user(to, std::as_bytes(std::span{&value, 1}));
}

/// The @p length bytes at user address @p from, as far as @p out has room for them: a name, or a
/// line to log, which can do without the rest.
[[nodiscard]] std::expected<std::string_view, Error> copy_string_from_user(
    memory::VirtualAddress from, std::size_t length, std::span<char> out) noexcept;

/// Access flags from user code: only those in @p allowed, plus User, which it always gets.
/// @return InvalidFlags for anything else.
[[nodiscard]] std::expected<memory::vmm::AccessFlags, Error> access_arg(
    std::uint64_t raw, memory::vmm::AccessFlags allowed) noexcept;

/// A PageSize, with 0 for 4 KiB pages.
[[nodiscard]] std::expected<memory::vmm::PageSize, Error> page_size_arg(std::uint64_t raw) noexcept;

[[nodiscard]] std::expected<memory::vmm::CacheMode, Error> cache_mode_arg(
    std::uint64_t raw) noexcept;

[[nodiscard]] std::expected<ipc::CapRights, Error> rights_arg(std::uint64_t raw) noexcept;

/// Where user code wants something mapped: 0 for anywhere, or a user address.
[[nodiscard]] std::expected<memory::VirtualAddress, Error> hint_arg(std::uint64_t raw) noexcept;

/// A size in bytes of user memory, which is never more than user space has room for. Saves the
/// address space from rounding something huge up past zero.
[[nodiscard]] std::expected<std::size_t, Error> size_arg(std::uint64_t raw) noexcept;
}  // namespace kernel::core::syscall
