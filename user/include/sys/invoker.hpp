#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <type_traits>

#include "sys/abi.hpp"
#include "sys/errors.hpp"

namespace user::sys {
namespace detail {
template <typename T>
concept HasRawMethod = requires(const std::remove_cvref_t<T>& val) {
  { val.raw() } noexcept -> std::integral;
};
}  // namespace detail

/// @brief Constrains types that can be passed directly in a 64-bit syscall argument register:
///        integers, pointers (including function pointers), enums, `nullptr`, and strong ABI
///        wrappers with `.raw()` (`CapId`, `MsgTag`).
template <typename T>
concept SyscallArg =
    (std::integral<std::remove_cvref_t<T>> || std::is_pointer_v<std::remove_cvref_t<T>> ||
     std::is_enum_v<std::remove_cvref_t<T>> ||
     std::same_as<std::remove_cvref_t<T>, std::nullptr_t> || detail::HasRawMethod<T>) &&
    (sizeof(std::remove_cvref_t<T>) <= sizeof(std::uintptr_t));

/// @brief Raw two-register output for syscalls that also return a value in `RDI`.
struct DualReturn {
  long rax{0};
  std::uint64_t rdi{0};
};

class SyscallInvoker {
  template <SyscallArg T>
  [[nodiscard, gnu::always_inline]] static constexpr std::uint64_t to_reg(const T arg) noexcept {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (std::same_as<CleanT, std::nullptr_t>) {
      return 0;
    } else if constexpr (detail::HasRawMethod<CleanT>) {
      return static_cast<std::uint64_t>(arg.raw());
    } else if constexpr (std::is_pointer_v<CleanT>) {
      return reinterpret_cast<std::uintptr_t>(arg);
    } else if constexpr (std::is_enum_v<CleanT>) {
      using U = std::make_unsigned_t<std::underlying_type_t<CleanT>>;
      return static_cast<std::uint64_t>(static_cast<U>(std::to_underlying(arg)));
    } else if constexpr (std::is_signed_v<CleanT>) {
      return static_cast<std::uint64_t>(static_cast<std::int64_t>(arg));
    } else {
      return static_cast<std::uint64_t>(arg);
    }
  }

  template <typename T>
  [[nodiscard, gnu::always_inline]] static constexpr T from_reg(const long raw) noexcept {
    if constexpr (std::is_void_v<T>) {
      return;
    } else if constexpr (std::same_as<T, CapId>) {
      return CapId{static_cast<std::uint32_t>(raw)};
    } else if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<T>(static_cast<std::uintptr_t>(raw));
    } else {
      return static_cast<T>(raw);
    }
  }

 public:
  template <SyscallArg... Args>
    requires(sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] static long invoke_raw(const Syscall sysno,
                                                           const Args... args) noexcept {
    constexpr std::size_t N = sizeof...(Args);
    long ret = std::to_underlying(sysno);

    if constexpr (N == 0) {
      asm volatile("syscall" : "+a"(ret) : : "rcx", "r11", "memory");
    } else if constexpr (N == 1) {
      const std::uint64_t a0 = to_reg(args...[0]);
      register std::uint64_t rdi asm("rdi") = a0;

      asm volatile("syscall" : "+a"(ret) : "r"(rdi) : "rcx", "r11", "memory");
    } else if constexpr (N == 2) {
      const std::uint64_t a0 = to_reg(args...[0]);
      const std::uint64_t a1 = to_reg(args...[1]);

      register std::uint64_t rdi asm("rdi") = a0;
      register std::uint64_t rsi asm("rsi") = a1;

      asm volatile("syscall" : "+a"(ret) : "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
    } else if constexpr (N == 3) {
      const std::uint64_t a0 = to_reg(args...[0]);
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);

      register std::uint64_t rdi asm("rdi") = a0;
      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;

      asm volatile("syscall" : "+a"(ret) : "r"(rdi), "r"(rsi), "d"(rdx) : "rcx", "r11", "memory");
    } else if constexpr (N == 4) {
      const std::uint64_t a0 = to_reg(args...[0]);
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);

      register std::uint64_t rdi asm("rdi") = a0;
      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;

      asm volatile("syscall"
                   : "+a"(ret)
                   : "r"(rdi), "r"(rsi), "d"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
    } else if constexpr (N == 5) {
      const std::uint64_t a0 = to_reg(args...[0]);
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);
      const std::uint64_t a4 = to_reg(args...[4]);

      register std::uint64_t rdi asm("rdi") = a0;
      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;
      register std::uint64_t r8 asm("r8") = a4;

      asm volatile("syscall"
                   : "+a"(ret)
                   : "r"(rdi), "r"(rsi), "d"(rdx), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
    } else if constexpr (N == 6) {
      const std::uint64_t a0 = to_reg(args...[0]);
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);
      const std::uint64_t a4 = to_reg(args...[4]);
      const std::uint64_t a5 = to_reg(args...[5]);

      register std::uint64_t rdi asm("rdi") = a0;
      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;
      register std::uint64_t r8 asm("r8") = a4;
      register std::uint64_t r9 asm("r9") = a5;

      asm volatile("syscall"
                   : "+a"(ret)
                   : "r"(rdi), "r"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
    }

    return ret;
  }

  template <SyscallArg... Args>
    requires(sizeof...(Args) >= 1 && sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] static auto invoke_dual_raw(const Syscall sysno,
                                                                const Args... args) noexcept
      -> DualReturn {
    constexpr std::size_t N = sizeof...(Args);
    long ret = std::to_underlying(sysno);
    std::uint64_t rdi_io = to_reg(args...[0]);

    if constexpr (N == 1) {
      asm volatile("syscall" : "+a"(ret), "+D"(rdi_io) : : "rcx", "r11", "memory");
    } else if constexpr (N == 2) {
      const std::uint64_t a1 = to_reg(args...[1]);
      register std::uint64_t rsi asm("rsi") = a1;

      asm volatile("syscall" : "+a"(ret), "+D"(rdi_io) : "r"(rsi) : "rcx", "r11", "memory");
    } else if constexpr (N == 3) {
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);

      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;

      asm volatile("syscall"
                   : "+a"(ret), "+D"(rdi_io)
                   : "r"(rsi), "d"(rdx)
                   : "rcx", "r11", "memory");
    } else if constexpr (N == 4) {
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);

      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;

      asm volatile("syscall"
                   : "+a"(ret), "+D"(rdi_io)
                   : "r"(rsi), "d"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
    } else if constexpr (N == 5) {
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);
      const std::uint64_t a4 = to_reg(args...[4]);

      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;
      register std::uint64_t r8 asm("r8") = a4;

      asm volatile("syscall"
                   : "+a"(ret), "+D"(rdi_io)
                   : "r"(rsi), "d"(rdx), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
    } else if constexpr (N == 6) {
      const std::uint64_t a1 = to_reg(args...[1]);
      const std::uint64_t a2 = to_reg(args...[2]);
      const std::uint64_t a3 = to_reg(args...[3]);
      const std::uint64_t a4 = to_reg(args...[4]);
      const std::uint64_t a5 = to_reg(args...[5]);

      register std::uint64_t rsi asm("rsi") = a1;
      register std::uint64_t rdx asm("rdx") = a2;
      register std::uint64_t r10 asm("r10") = a3;
      register std::uint64_t r8 asm("r8") = a4;
      register std::uint64_t r9 asm("r9") = a5;

      asm volatile("syscall"
                   : "+a"(ret), "+D"(rdi_io)
                   : "r"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
    }

    return {.rax = ret, .rdi = rdi_io};
  }

  /// @brief Invokes a syscall and returns `Result<long>`.
  template <SyscallArg... Args>
    requires(sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] static auto invoke(const Syscall sysno,
                                                       const Args... args) noexcept
      -> Result<long> {
    const long ret = invoke_raw(sysno, args...);
    if (ret < 0) [[unlikely]] {
      return std::unexpected(decode_error(ret));
    }
    return ret;
  }

  /// @brief Invokes a syscall and converts a non-negative return value directly to `Result<R>`
  ///        (`void`, `CapId`, pointer, or integer).
  template <typename R, SyscallArg... Args>
    requires(sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] static auto invoke_as(const Syscall sysno,
                                                          const Args... args) noexcept
      -> Result<R> {
    const long ret = invoke_raw(sysno, args...);
    if (ret < 0) [[unlikely]] {
      return std::unexpected(decode_error(ret));
    }
    if constexpr (std::is_void_v<R>) {
      return {};
    } else {
      return from_reg<R>(ret);
    }
  }

  /// @brief Invokes a dual-return syscall (`RAX` + `RDI`) and returns `Result<DualReturn>`.
  template <SyscallArg... Args>
    requires(sizeof...(Args) >= 1 && sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] static auto invoke_dual(const Syscall sysno,
                                                            const Args... args) noexcept
      -> Result<DualReturn> {
    const DualReturn res = invoke_dual_raw(sysno, args...);
    if (res.rax < 0) [[unlikely]] {
      return std::unexpected(decode_error(res.rax));
    }

    return res;
  }

  /// @brief Sends a register-packed `Message` (`IpcSend`, `IpcTrySend`, `IpcReply`).
  [[nodiscard, gnu::always_inline]] static auto invoke_ipc_send(const Syscall sysno,
                                                                const CapId target,
                                                                const Message& msg) noexcept
      -> Result<void> {
    long rax = std::to_underlying(sysno);
    const std::uint64_t rdi = target.raw();
    const std::uint64_t rsi = msg.tag.raw();
    const std::uint64_t rdx = msg.words[0];
    register std::uint64_t r10 asm("r10") = msg.words[1];
    register std::uint64_t r8 asm("r8") = msg.words[2];
    register std::uint64_t r9 asm("r9") = msg.words[3];
    register std::uint64_t r12 asm("r12") = msg.words[4];
    register std::uint64_t r13 asm("r13") = msg.words[5];

    asm volatile("syscall"
                 : "+a"(rax)
                 : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9), "r"(r12), "r"(r13)
                 : "rcx", "r11", "memory");
    if (rax < 0) [[unlikely]] {
      return std::unexpected(decode_error(rax));
    }
    return {};
  }

  /// @brief Receives a register-packed `Message` (`IpcRecv`, `IpcTryRecv`).
  [[nodiscard, gnu::always_inline]] static auto invoke_ipc_recv(const Syscall sysno,
                                                                const CapId endpoint) noexcept
      -> Result<Message> {
    long rax = std::to_underlying(sysno);
    std::uint64_t rdi = endpoint.raw();
    std::uint64_t rsi = 0;
    std::uint64_t rdx = 0;

    register std::uint64_t r10 asm("r10") = 0;
    register std::uint64_t r8 asm("r8") = 0;
    register std::uint64_t r9 asm("r9") = 0;
    register std::uint64_t r12 asm("r12") = 0;
    register std::uint64_t r13 asm("r13") = 0;

    asm volatile("syscall"
                 : "+a"(rax), "+D"(rdi), "=S"(rsi), "=d"(rdx), "=r"(r10), "=r"(r8), "=r"(r9),
                   "=r"(r12), "=r"(r13)
                 :
                 : "rcx", "r11", "memory");
    if (rax < 0) [[unlikely]] {
      return std::unexpected(decode_error(rax));
    }

    Message out{};
    out.badge = rdi;
    out.tag = MsgTag{rsi};
    out.words = {rdx, r10, r8, r9, r12, r13};
    return out;
  }

  /// @brief Sends @p outgoing and receives @p incoming in registers (`IpcCall`, `IpcReplyRecv`).
  [[nodiscard, gnu::always_inline]] static auto invoke_ipc_call(const Syscall sysno,
                                                                const CapId endpoint,
                                                                const Message& outgoing) noexcept
      -> Result<Message> {
    long rax = std::to_underlying(sysno);
    std::uint64_t rdi = endpoint.raw();
    std::uint64_t rsi = outgoing.tag.raw();
    std::uint64_t rdx = outgoing.words[0];
    register std::uint64_t r10 asm("r10") = outgoing.words[1];
    register std::uint64_t r8 asm("r8") = outgoing.words[2];
    register std::uint64_t r9 asm("r9") = outgoing.words[3];
    register std::uint64_t r12 asm("r12") = outgoing.words[4];
    register std::uint64_t r13 asm("r13") = outgoing.words[5];

    asm volatile("syscall"
                 : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9),
                   "+r"(r12), "+r"(r13)
                 :
                 : "rcx", "r11", "memory");
    if (rax < 0) [[unlikely]] {
      return std::unexpected(decode_error(rax));
    }

    Message incoming{};
    incoming.badge = rdi;
    incoming.tag = MsgTag{rsi};
    incoming.words = {rdx, r10, r8, r9, r12, r13};
    return incoming;
  }

  template <SyscallArg... Args>
    requires(sizeof...(Args) <= 6)
  [[nodiscard, gnu::always_inline]] auto operator()(const Syscall sysno,
                                                    const Args... args) const noexcept
      -> Result<long> {
    return invoke(sysno, args...);
  }
};

inline constexpr SyscallInvoker invoke_syscall{};
}  // namespace user::sys