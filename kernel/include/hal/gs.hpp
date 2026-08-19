#pragma once

#include <cstddef>
#include <cstdint>

#include <bit>

#define READ_PCP(member) ::kernel::hw::gs::read<offsetof(PerCpu, member), decltype(PerCpu::member)>()
#define WRITE_PCP(member, val) ::kernel::hw::gs::write<offsetof(PerCpu, member), decltype(PerCpu::member)>(val)
#define INC_PCP(member) ::kernel::hw::gs::inc<offsetof(PerCpu, member), sizeof(decltype(PerCpu::member))>()
#define DEC_PCP(member) ::kernel::hw::gs::dec<offsetof(PerCpu, member), sizeof(decltype(PerCpu::member))>()

namespace kernel::hw::gs {
template <std::size_t Offset, typename T> [[nodiscard, gnu::always_inline]] T read() noexcept {
  if constexpr (sizeof(T) == 8) {
    std::uint64_t val;
    asm volatile("movq %%gs:%c1, %0" : "=r"(val) : "i"(Offset));
    return std::bit_cast<T>(val);
  } else if constexpr (sizeof(T) == 4) {
    std::uint32_t val;
    asm volatile("movl %%gs:%c1, %0" : "=r"(val) : "i"(Offset));
    return std::bit_cast<T>(val);
  } else if constexpr (sizeof(T) == 2) {
    std::uint16_t val;
    asm volatile("movw %%gs:%c1, %0" : "=r"(val) : "i"(Offset));
    return std::bit_cast<T>(val);
  } else if constexpr (sizeof(T) == 1) {
    std::uint8_t val;
    asm volatile("movb %%gs:%c1, %0" : "=r"(val) : "i"(Offset));
    return std::bit_cast<T>(val);
  }
}

template <std::size_t Offset, typename T> [[gnu::always_inline]] void write(T value) noexcept {
  asm volatile("" ::: "memory");
  if constexpr (sizeof(T) == 8) {
    asm volatile("movq %0, %%gs:%c1" : : "re"(__builtin_bit_cast(std::uint64_t, value)), "i"(Offset) : "memory");
  } else if constexpr (sizeof(T) == 4) {
    asm volatile("movl %0, %%gs:%c1" : : "re"(__builtin_bit_cast(std::uint32_t, value)), "i"(Offset) : "memory");
  } else if constexpr (sizeof(T) == 2) {
    asm volatile("movw %0, %%gs:%c1" : : "re"(__builtin_bit_cast(std::uint16_t, value)), "i"(Offset) : "memory");
  } else if constexpr (sizeof(T) == 1) {
    asm volatile("movb %0, %%gs:%c1" : : "re"(__builtin_bit_cast(std::uint8_t, value)), "i"(Offset) : "memory");
  }
}

template <std::size_t Offset, std::size_t Size> [[gnu::always_inline]] void inc() noexcept {
  if constexpr (Size == 8) {
    asm volatile("incq %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 4) {
    asm volatile("incl %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 2) {
    asm volatile("incw %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 1) {
    asm volatile("incb %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  }
}

template <std::size_t Offset, std::size_t Size> [[gnu::always_inline]] void dec() noexcept {
  if constexpr (Size == 8) {
    asm volatile("decq %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 4) {
    asm volatile("decl %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 2) {
    asm volatile("devw %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  } else if constexpr (Size == 1) {
    asm volatile("decb %%gs:%c0" : : "i"(Offset) : "memory", "cc");
  }
}

template <std::size_t Offset, std::size_t Size> [[gnu::always_inline]] void add(std::uint32_t amount) noexcept {
  if constexpr (Size == 1) {
    asm volatile("addb %1, %%gs:%c0" : : "i"(Offset), "i"(amount) : "memory", "cc");
  } else if constexpr (Size == 4) {
    asm volatile("addl %1, %%gs:%c0" : : "i"(Offset), "i"(amount) : "memory", "cc");
  }
}

template <std::size_t Offset, std::size_t Size> [[gnu::always_inline]] void sub(std::uint32_t amount) noexcept {
  if constexpr (Size == 1) {
    asm volatile("subb %1, %%gs:%c0" : : "i"(Offset), "i"(amount) : "memory", "cc");
  }
}
} // namespace kernel::hw::gs