#pragma once

#include <compare>
#include <cstdint>
#include <optional>

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
  constexpr PhysicalAddress() noexcept : m_address{0} {}
  constexpr explicit PhysicalAddress(const ValueType address) noexcept : m_address{address} {}
  explicit PhysicalAddress(std::nullptr_t) = delete;
  template <typename T> explicit PhysicalAddress(T *) = delete;

  [[nodiscard]] constexpr ValueType value() const noexcept { return m_address; }

  [[nodiscard]] friend constexpr auto operator<=>(PhysicalAddress, PhysicalAddress) noexcept = default;

  [[nodiscard]] friend constexpr PhysicalAddress operator+(const PhysicalAddress addr,
                                                           const OffsetType offset) noexcept {
    return PhysicalAddress{addr.m_address + offset};
  }

  [[nodiscard]] friend constexpr PhysicalAddress operator+(const OffsetType offset,
                                                           const PhysicalAddress addr) noexcept {
    return PhysicalAddress{addr.m_address + offset};
  }

  [[nodiscard]] friend constexpr PhysicalAddress operator-(const PhysicalAddress addr,
                                                           const OffsetType offset) noexcept {
    return PhysicalAddress{addr.m_address - offset};
  }

  [[nodiscard]] friend constexpr OffsetType operator-(const PhysicalAddress lhs, const PhysicalAddress rhs) noexcept {
    return lhs.m_address - rhs.m_address;
  }

  constexpr PhysicalAddress &operator+=(const PhysicalAddress other) noexcept {
    m_address += other.m_address;
    return *this;
  }

  constexpr PhysicalAddress &operator-=(const PhysicalAddress other) noexcept {
    m_address -= other.m_address;
    return *this;
  }

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

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_address != 0; }

  [[nodiscard]] constexpr VirtualAddress canonicalize() const noexcept {
    const auto signed_addr = static_cast<std::int64_t>(m_address << 16) >> 16;
    return VirtualAddress{static_cast<ValueType>(signed_addr)};
  }

  [[nodiscard]] constexpr ValueType page_offset(const std::size_t align = 0x1000) const noexcept {
    return m_address & (align - 1);
  }

  [[nodiscard]] constexpr ValueType pml1_index() const noexcept { return (m_address >> 12) & 0x1FF; }
  [[nodiscard]] constexpr ValueType pml2_index() const noexcept { return (m_address >> 21) & 0x1FF; }
  [[nodiscard]] constexpr ValueType pml3_index() const noexcept { return (m_address >> 30) & 0x1FF; }
  [[nodiscard]] constexpr ValueType pml4_index() const noexcept { return (m_address >> 39) & 0x1FF; }
  [[nodiscard]] constexpr ValueType pml5_index() const noexcept { return (m_address >> 48) & 0x1FF; }

  [[nodiscard]] friend constexpr auto operator<=>(VirtualAddress, VirtualAddress) noexcept = default;

  [[nodiscard]] friend constexpr VirtualAddress operator+(const VirtualAddress addr, const OffsetType offset) noexcept {
    return VirtualAddress{addr.m_address + offset};
  }

  // FIXED: Added missing subtraction operator for offsets
  [[nodiscard]] friend constexpr VirtualAddress operator-(const VirtualAddress addr, const OffsetType offset) noexcept {
    return VirtualAddress{addr.m_address - offset};
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

  constexpr VirtualAddress &operator+=(const OffsetType size) noexcept {
    this->m_address += size;
    return *this;
  }

  constexpr VirtualAddress &operator-=(const OffsetType size) noexcept {
    this->m_address -= size;
    return *this;
  }
};

class DirectMap {
public:
  inline static std::uintptr_t s_hhdm_base{0};

  static void initialize(const std::uintptr_t base) noexcept { s_hhdm_base = base; }

  static VirtualAddress phys_to_virt(const PhysicalAddress addr) noexcept {
    return VirtualAddress{addr.value() + s_hhdm_base};
  }

  static std::optional<PhysicalAddress> virt_to_phys(const VirtualAddress addr) noexcept {
    if (addr.value() < s_hhdm_base) {
      return std::nullopt;
    }
    return PhysicalAddress{addr.value() - s_hhdm_base};
  }
};
} // namespace kernel::memory

namespace klib {
template <> struct formatter<kernel::memory::PhysicalAddress> {
  template <typename Sink>
  static constexpr void format(Sink &buf, const kernel::memory::PhysicalAddress &address,
                               const FormatSpec &spec) noexcept {
    FormatSpec hex_spec = spec;
    hex_spec.base = 16;
    hex_spec.alt_form = true;
    formatter<std::uintptr_t>::format(buf, address.value(), hex_spec);
  }
};

template <> struct formatter<kernel::memory::VirtualAddress> {
  template <typename Sink>
  static constexpr void format(Sink &buf, const kernel::memory::VirtualAddress &address,
                               const FormatSpec &spec) noexcept {
    FormatSpec hex_spec = spec;
    hex_spec.base = 16;
    hex_spec.alt_form = true;
    formatter<std::uintptr_t>::format(buf, address.value(), hex_spec);
  }
};
} // namespace klib