#pragma once

#include <compare>
#include <cstdint>
#include <optional>

#include "memory.hpp"
#include "utils/maths.hpp"

#include <kformat/formatter.hpp>

namespace kernel::memory {
class PhysicalAddress {
public:
  using ValueType = std::uintptr_t;
  using OffsetType = std::ptrdiff_t;

private:
  ValueType m_address;

public:
  // Initializes to 0
  constexpr PhysicalAddress() noexcept : m_address{0} {}

  constexpr explicit PhysicalAddress(const ValueType address) noexcept
      : m_address{utils::maths::align_down(address, PAGE_SIZE)} {}

  explicit PhysicalAddress(std::nullptr_t) = delete;

  template <typename T> explicit PhysicalAddress(T *) = delete;

  [[nodiscard]] constexpr ValueType value() const noexcept { return m_address; }

  [[nodiscard]] friend constexpr auto operator<=>(PhysicalAddress, PhysicalAddress) noexcept = default;

  // Address + Offset = Address
  [[nodiscard]] friend constexpr PhysicalAddress operator+(const PhysicalAddress addr,
                                                           const OffsetType offset) noexcept {
    return PhysicalAddress{addr.m_address + offset};
  }

  // Offset + Address = Address
  [[nodiscard]] friend constexpr PhysicalAddress operator+(const OffsetType offset,
                                                           const PhysicalAddress addr) noexcept {
    return PhysicalAddress{addr.m_address + offset};
  }

  // Address - Offset = Address
  [[nodiscard]] friend constexpr PhysicalAddress operator-(const PhysicalAddress addr,
                                                           const OffsetType offset) noexcept {
    return PhysicalAddress{addr.m_address - offset};
  }

  // Address - Address = Offset (Distance between two physical addresses)
  [[nodiscard]] friend constexpr OffsetType operator-(const PhysicalAddress lhs, const PhysicalAddress rhs) noexcept {
    return lhs.m_address - rhs.m_address;
  }

  // Compound assignments
  constexpr PhysicalAddress &operator+=(const OffsetType offset) noexcept {
    m_address += offset;
    return *this;
  }

  constexpr PhysicalAddress &operator-=(const OffsetType offset) noexcept {
    m_address -= offset;
    return *this;
  }

  [[nodiscard]] friend constexpr PhysicalAddress operator&(const PhysicalAddress addr, const ValueType mask) noexcept {
    return PhysicalAddress{addr.m_address & mask};
  }

  [[nodiscard]] constexpr PhysicalAddress align_up(const ValueType alignment) const noexcept {
    return PhysicalAddress{utils::maths::align_up(m_address, alignment)};
  }

  [[nodiscard]] constexpr PhysicalAddress align_down(const ValueType alignment) const noexcept {
    return PhysicalAddress{utils::maths::align_down(m_address, alignment)};
  }

  [[nodiscard]] constexpr bool is_aligned(const ValueType alignment) const noexcept {
    return utils::maths::is_aligned(m_address, alignment);
  }
};

class VirtualAddress {
public:
  using ValueType = std::uintptr_t;
  using OffsetType = std::ptrdiff_t;

private:
  ValueType m_address;

public:
  constexpr VirtualAddress() noexcept : m_address{0} {}
  constexpr explicit VirtualAddress(const ValueType address) noexcept : m_address{address} {}

  template <typename T> explicit VirtualAddress(T *ptr) noexcept : m_address{reinterpret_cast<ValueType>(ptr)} {}

  constexpr explicit VirtualAddress(std::nullptr_t) noexcept : m_address{0} {}

  template <typename T> [[nodiscard]] T *as() const noexcept { return reinterpret_cast<T *>(m_address); }

  [[nodiscard]] constexpr ValueType value() const noexcept { return m_address; }

  [[nodiscard]] constexpr ValueType page_offset() const noexcept { return m_address & 0xFFF; }

  [[nodiscard]] constexpr ValueType pml1_index() const noexcept { return (m_address >> 12) & 0x1FF; }

  [[nodiscard]] constexpr ValueType pml2_index() const noexcept { return (m_address >> 21) & 0x1FF; }

  [[nodiscard]] constexpr ValueType pml3_index() const noexcept { return (m_address >> 30) & 0x1FF; }

  [[nodiscard]] constexpr ValueType pml4_index() const noexcept { return (m_address >> 39) & 0x1FF; }

  [[nodiscard]] constexpr ValueType pml5_index() const noexcept { return (m_address >> 48) & 0x1FF; }

  [[nodiscard]] friend constexpr auto operator<=>(VirtualAddress, VirtualAddress) noexcept = default;

  [[nodiscard]] friend constexpr VirtualAddress operator+(const VirtualAddress addr, const OffsetType offset) noexcept {
    return VirtualAddress{addr.m_address + offset};
  }

  [[nodiscard]] friend constexpr OffsetType operator-(const VirtualAddress lhs, const VirtualAddress rhs) noexcept {
    return lhs.m_address - rhs.m_address;
  }

  [[nodiscard]] constexpr VirtualAddress align_down(const ValueType alignment) const noexcept {
    return VirtualAddress{utils::maths::align_down(m_address, alignment)};
  }

  [[nodiscard]] constexpr VirtualAddress align_up(const ValueType alignment) const noexcept {
    return VirtualAddress{utils::maths::align_up(m_address, alignment)};
  }

  [[nodiscard]] constexpr bool is_aligned(const ValueType alignment) const noexcept {
    return utils::maths::is_aligned(m_address, alignment);
  }
};

class DirectMap {
  inline static std::ptrdiff_t s_hhdm_offset;

public:
  static void initialize(const std::ptrdiff_t offset) noexcept { s_hhdm_offset = offset; }

  static constexpr VirtualAddress phys_to_virt(const PhysicalAddress addr) noexcept {
    return VirtualAddress{addr.value() + s_hhdm_offset};
  }

  static constexpr std::optional<PhysicalAddress> virt_to_phys(const VirtualAddress addr) noexcept {
    if (addr.value() < static_cast<std::uintptr_t>(s_hhdm_offset)) {
      return std::nullopt;
    }

    return PhysicalAddress{addr.value() - s_hhdm_offset};
  }
};
} // namespace kernel::memory

namespace klib {
template <> struct formatter<kernel::memory::PhysicalAddress> {
  template <typename Sink>
  static constexpr void format(Sink &buf, const kernel::memory::PhysicalAddress &address,
                               const FormatSpec &spec) noexcept {
    formatter<void *>::format(buf, reinterpret_cast<void *>(address.value()), spec);
  }
};

template <> struct formatter<kernel::memory::VirtualAddress> {
  template <typename Sink>
  static constexpr void format(Sink &buf, const kernel::memory::VirtualAddress &address,
                               const FormatSpec &spec) noexcept {
    formatter<void *>::format(buf, reinterpret_cast<void *>(address.value()), spec);
  }
};
} // namespace klib