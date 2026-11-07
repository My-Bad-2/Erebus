/// @file address.hpp
/// @brief Strongly typed physical/virtual addresses and the higher-half direct map.
///
/// Physical and virtual addresses are both just integers to the CPU, which makes them very easy
/// to mix up. The two wrappers here are deliberately not convertible into each other: the only
/// sanctioned bridge is DirectMap, which translates between a physical address and its alias in
/// the HHDM.

#pragma once

#include <compare>
#include <cstdint>
#include <kformat/formatters.hpp>
#include <optional>

#include "utils/maths.hpp"

namespace kernel::memory {
/// @brief A physical (bus) address.
///
/// Arithmetic is limited to adding or subtracting byte offsets and taking the distance between
/// two addresses. Constructing one from a pointer is explicitly deleted: a pointer is always a
/// virtual address, and silently treating it as physical is a classic source of bugs.
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
  template <typename T>
  explicit PhysicalAddress(T*) = delete;

  [[nodiscard]] constexpr ValueType value() const noexcept { return m_address; }

  explicit operator std::uint64_t() const noexcept { return m_address; }

  [[nodiscard]] friend constexpr auto operator<=>(PhysicalAddress,
                                                  PhysicalAddress) noexcept = default;

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

  /// @brief Distance in bytes between two physical addresses.
  [[nodiscard]] friend constexpr OffsetType operator-(const PhysicalAddress lhs,
                                                      const PhysicalAddress rhs) noexcept {
    return lhs.m_address - rhs.m_address;
  }

  constexpr PhysicalAddress& operator+=(const OffsetType offset) noexcept {
    m_address += offset;
    return *this;
  }

  constexpr PhysicalAddress& operator-=(const OffsetType offset) noexcept {
    m_address -= offset;
    return *this;
  }

  [[nodiscard]] friend constexpr PhysicalAddress operator&(const PhysicalAddress addr,
                                                           const ValueType mask) noexcept {
    return PhysicalAddress{addr.m_address & mask};
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr PhysicalAddress align_up(const ValueType alignment) const noexcept {
    return PhysicalAddress{utils::maths::align_up(m_address, alignment)};
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr PhysicalAddress align_down(const ValueType alignment) const noexcept {
    return PhysicalAddress{utils::maths::align_down(m_address, alignment)};
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr bool is_aligned(const ValueType alignment) const noexcept {
    return utils::maths::is_aligned(m_address, alignment);
  }
};

/// @brief A linear (virtual) address.
///
/// Besides the usual arithmetic this knows how to split itself into page-table indices. The
/// index helpers follow the Intel naming where PML1 is the page table and PML4/PML5 is the root,
/// so `pml<N>_index()` is the slot used at paging level N.
class VirtualAddress {
 public:
  using ValueType = std::uintptr_t;
  using OffsetType = std::ptrdiff_t;

 private:
  ValueType m_address;

 public:
  constexpr VirtualAddress() noexcept : m_address{0} {}
  constexpr explicit VirtualAddress(const ValueType address) noexcept : m_address{address} {}

  template <typename T>
  explicit VirtualAddress(T* ptr) noexcept : m_address{reinterpret_cast<ValueType>(ptr)} {}
  constexpr explicit VirtualAddress(std::nullptr_t) noexcept : m_address{0} {}

  template <typename T>
  [[nodiscard]] T* as() const noexcept {
    return reinterpret_cast<T*>(m_address);
  }
  [[nodiscard]] constexpr ValueType value() const noexcept { return m_address; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_address != 0; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return m_address == 0; }

  /// @brief True for addresses in the kernel (upper) half of the canonical address space.
  ///
  /// Bit 63 is set for every canonical higher-half address with both 4- and 5-level paging, so
  /// this works without knowing which one is active.
  [[nodiscard]] constexpr bool is_upper_half() const noexcept { return (m_address >> 63) != 0; }

  /// @brief Sign-extends bit 47 into the upper bits (4-level canonical form).
  [[nodiscard]] constexpr VirtualAddress canonicalize() const noexcept {
    const auto signed_addr = static_cast<std::int64_t>(m_address << 16) >> 16;
    return VirtualAddress{static_cast<ValueType>(signed_addr)};
  }

  /// @brief Offset of this address inside a naturally aligned block of @p align bytes.
  [[nodiscard]] constexpr ValueType page_offset(const std::size_t align = 0x1000) const noexcept {
    return m_address & (align - 1);
  }

  [[nodiscard]] constexpr ValueType pml1_index() const noexcept {
    return (m_address >> 12) & 0x1FF;
  }
  [[nodiscard]] constexpr ValueType pml2_index() const noexcept {
    return (m_address >> 21) & 0x1FF;
  }
  [[nodiscard]] constexpr ValueType pml3_index() const noexcept {
    return (m_address >> 30) & 0x1FF;
  }
  [[nodiscard]] constexpr ValueType pml4_index() const noexcept {
    return (m_address >> 39) & 0x1FF;
  }
  [[nodiscard]] constexpr ValueType pml5_index() const noexcept {
    return (m_address >> 48) & 0x1FF;
  }

  [[nodiscard]] friend constexpr auto operator<=>(VirtualAddress,
                                                  VirtualAddress) noexcept = default;

  [[nodiscard]] friend constexpr VirtualAddress operator+(const VirtualAddress addr,
                                                          const OffsetType offset) noexcept {
    return VirtualAddress{addr.m_address + offset};
  }

  [[nodiscard]] friend constexpr VirtualAddress operator-(const VirtualAddress addr,
                                                          const OffsetType offset) noexcept {
    return VirtualAddress{addr.m_address - offset};
  }

  /// @brief Distance in bytes between two virtual addresses.
  [[nodiscard]] friend constexpr OffsetType operator-(const VirtualAddress lhs,
                                                      const VirtualAddress rhs) noexcept {
    return lhs.m_address - rhs.m_address;
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr VirtualAddress align_down(const ValueType alignment) const noexcept {
    return VirtualAddress{utils::maths::align_down(m_address, alignment)};
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr VirtualAddress align_up(const ValueType alignment) const noexcept {
    return VirtualAddress{utils::maths::align_up(m_address, alignment)};
  }

  /// @pre @p alignment is a power of two.
  [[nodiscard]] constexpr bool is_aligned(const ValueType alignment) const noexcept {
    return utils::maths::is_aligned(m_address, alignment);
  }

  constexpr VirtualAddress& operator+=(const OffsetType size) noexcept {
    m_address += size;
    return *this;
  }

  constexpr VirtualAddress& operator-=(const OffsetType size) noexcept {
    m_address -= size;
    return *this;
  }
};

/// @brief Translation between physical memory and its higher-half direct map (HHDM).
///
/// Every byte of physical memory the kernel manages is reachable at `phys + base()`. The base is
/// handed to us by Limine and recorded once during memory::initialize(); after that both
/// directions are a single add/subtract.
class DirectMap {
  inline static std::uintptr_t s_hhdm_base{0};

 public:
  static void initialize(const std::uintptr_t base) noexcept { s_hhdm_base = base; }

  [[nodiscard]] static std::uintptr_t base() noexcept { return s_hhdm_base; }

  [[nodiscard]] static VirtualAddress phys_to_virt(const PhysicalAddress addr) noexcept {
    return VirtualAddress{addr.value() + s_hhdm_base};
  }

  /// @brief Inverse of phys_to_virt().
  /// @return std::nullopt if @p addr lies below the direct map. Addresses above the end of
  ///         physical memory are not rejected here; callers that feed the result to the PMM get
  ///         a bounds check from pfn_to_page().
  [[nodiscard]] static std::optional<PhysicalAddress> virt_to_phys(
      const VirtualAddress addr) noexcept {
    if (addr.value() < s_hhdm_base) {
      return std::nullopt;
    }
    return PhysicalAddress{addr.value() - s_hhdm_base};
  }
};
}  // namespace kernel::memory

namespace klib {
template <>
struct formatter<kernel::memory::PhysicalAddress> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const kernel::memory::PhysicalAddress& address,
                               const FormatSpec& spec) noexcept {
    FormatSpec hex_spec = spec;
    hex_spec.base = 16;
    hex_spec.alt_form = true;
    formatter<std::uintptr_t>::format(buf, address.value(), hex_spec);
  }
};

template <>
struct formatter<kernel::memory::VirtualAddress> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const kernel::memory::VirtualAddress& address,
                               const FormatSpec& spec) noexcept {
    FormatSpec hex_spec = spec;
    hex_spec.base = 16;
    hex_spec.alt_form = true;
    formatter<std::uintptr_t>::format(buf, address.value(), hex_spec);
  }
};
}  // namespace klib
