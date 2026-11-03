#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

#include "memory/address.hpp"

namespace kernel::hw {
enum class IoType : std::uint8_t {
  PMIO,  // Port-Mapped I/O
  MMIO   // Memory-Mapped I/O
};

namespace mmio {
/// @brief Reads a device register. A single access of exactly `sizeof(T)` bytes.
///
/// Device memory is mapped uncacheable, and x86 neither reorders nor speculates UC accesses, so
/// no fence is needed against other MMIO or against ordinary memory accesses. The asm still
/// keeps the *compiler* from moving ordinary loads and stores across the access.
template <std::unsigned_integral T>
[[nodiscard, gnu::always_inline]] inline T read(const std::uintptr_t addr) noexcept {
  T val;
  asm volatile("mov %1, %0"
               : "=r"(val)
               : "m"(*reinterpret_cast<const volatile T*>(addr))
               : "memory");
  return val;
}

/// @brief Writes a device register. A single access of exactly `sizeof(T)` bytes.
template <std::unsigned_integral T>
[[gnu::always_inline]] inline void write(const std::uintptr_t addr, const T val) noexcept {
  asm volatile("mov %1, %0" : "=m"(*reinterpret_cast<volatile T*>(addr)) : "r"(val) : "memory");
}

/// @brief Orders write-combining and non-temporal stores before later stores.
///
/// Only needed when a device reads memory that was written through a WC mapping or with
/// streaming stores (a framebuffer, say) before being told about it with an MMIO write.
[[gnu::always_inline]] inline void write_barrier() noexcept { asm volatile("sfence" ::: "memory"); }
}  // namespace mmio

class MmioResource {
  memory::VirtualAddress m_base{};

 public:
  constexpr MmioResource() noexcept = default;
  constexpr explicit MmioResource(const memory::VirtualAddress base) noexcept : m_base{base} {}

  constexpr void set_base(memory::VirtualAddress addr) noexcept { m_base = addr; }

  template <std::unsigned_integral T>
  [[gnu::always_inline]] void write(const std::size_t offset, const T val) const noexcept {
    mmio::write<T>((m_base + offset).value(), val);
  }

  template <std::unsigned_integral T>
  [[nodiscard, gnu::always_inline]] T read(const std::size_t offset) const noexcept {
    return mmio::read<T>((m_base + offset).value());
  }
};

class IoResource {
  std::uintptr_t m_base{};
  IoType m_type{};
  std::uint8_t m_stride{};

 public:
  constexpr IoResource() noexcept = default;

  constexpr explicit IoResource(const IoType type, std::uintptr_t base,
                                const std::uint8_t stride = 1) noexcept
      : m_base(base), m_type(type), m_stride(stride) {}

  template <std::unsigned_integral T>
  [[gnu::always_inline]] void write(std::size_t offset, T val) const noexcept {
    if (m_type == IoType::PMIO) {
      const auto port = static_cast<std::uint16_t>(m_base + (offset * m_stride));

      if constexpr (sizeof(T) == 1) {
        asm volatile("outb %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 2) {
        asm volatile("outw %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 4) {
        asm volatile("outl %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else {
        static_assert(false, "x86-64 doesn't support 64-bit Port I/O");
      }
    } else {
      mmio::write<T>(m_base + (offset * m_stride), val);
    }
  }

  template <std::unsigned_integral T>
  [[nodiscard, gnu::always_inline]] T read(std::size_t offset) const noexcept {
    if (m_type == IoType::PMIO) {
      auto port = static_cast<std::uint16_t>(m_base + (offset * m_stride));
      T ret;

      if constexpr (sizeof(T) == 1) {
        asm volatile("inb %w1, %0" : "=a"(ret) : "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 2) {
        asm volatile("inw %w1, %0" : "=a"(ret) : "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 4) {
        asm volatile("inl %w1, %0" : "=a"(ret) : "Nd"(port) : "memory");
      } else {
        static_assert(false, "x86-64 doesn't support 64-bit Port I/O");
      }

      return ret;
    } else {
      return mmio::read<T>(m_base + (offset * m_stride));
    }
  }
};
}  // namespace kernel::hw
