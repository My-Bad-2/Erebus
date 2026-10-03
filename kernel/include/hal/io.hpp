#pragma once

#include "memory/address.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace kernel::hw {
enum class IoType : std::uint8_t {
  PMIO, // Port-Mapped I/O
  MMIO  // Memory-Mapped I/O
};

class MmioResource {
  memory::VirtualAddress m_base{};

public:
  constexpr MmioResource() noexcept = default;
  constexpr explicit MmioResource(const memory::VirtualAddress base) noexcept : m_base{base} {}

  constexpr void set_base(memory::VirtualAddress addr) noexcept { m_base = addr; }

  template <std::unsigned_integral T>
  [[gnu::always_inline]] void write(std::size_t offset, std::size_t val) const noexcept {
    // Flush write-combining memory buffers
    const memory::VirtualAddress addr = m_base + offset;
    // sfence guarantees writes aren't held in write-combining buffers indefinitely
    asm volatile("sfence" : /* No Output */ : /* No Input */ : "memory");
    *reinterpret_cast<volatile T *>(addr.value()) = val;
  }

  template <std::unsigned_integral T> [[nodiscard, gnu::always_inline]] T read(std::size_t offset) const noexcept {
    const memory::VirtualAddress addr = m_base + offset;
    // lfence guarantees no speculative out-of-order execution precedes the read
    asm volatile("lfence" : /* No Output */ : /* No Input */ : "memory");
    return *reinterpret_cast<volatile T *>(addr.value());
  }
};

class IoResource {
  std::uintptr_t m_base{};
  IoType m_type{};
  std::uint8_t m_stride{};

public:
  constexpr IoResource() noexcept = default;

  constexpr explicit IoResource(const IoType type, std::uintptr_t base, const std::uint8_t stride = 1) noexcept
      : m_base(base), m_type(type), m_stride(stride) {}

  template <std::unsigned_integral T> [[gnu::always_inline]] void write(std::size_t offset, T val) const noexcept {
    if (m_type == IoType::PMIO) {
      const auto port = static_cast<std::uint16_t>(m_base + (offset * m_stride));

      if constexpr (sizeof(T) == 1) {
        asm volatile("outb %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 2) {
        asm volatile("outw %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else if constexpr (sizeof(T) == 4) {
        asm volatile("outl %0, %w1" ::"a"(val), "Nd"(port) : "memory");
      } else {
        static_assert(sizeof(T) != sizeof(T), "x86-64 doesn't support 64-bit Port I/O");
      }
    } else {
      // Flush write-combining memory buffers
      asm volatile("sfence" : /* No Output */ : /* No Input */ : "memory");
      *reinterpret_cast<volatile T *>(m_base + (offset * m_stride)) = val;
    }
  }

  template <std::unsigned_integral T> [[nodiscard, gnu::always_inline]] T read(std::size_t offset) const noexcept {
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
        static_assert(sizeof(T) != sizeof(T), "x86-64 doesn't support 64-bit Port I/O");
      }

      return ret;
    } else {
      // Prevent speculative out-of-order reading
      asm volatile("lfence" : /* No Output */ : /* No Input */ : "memory");
      return *reinterpret_cast<volatile T *>(m_base + (offset * m_stride));
    }
  }
};
} // namespace kernel::hw
