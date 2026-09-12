#pragma once

// Field macros (multi-bit values)
#define BF_RO(Type, Name, Offset, Width)                                                                               \
  [[nodiscard]] constexpr Type get_##Name() const noexcept {                                                           \
    using BT = decltype(m_data);                                                                                       \
    static_assert(Offset + Width <= sizeof(BT) * 8, "Bitfield [" #Name "] exceeds storage width");                     \
    constexpr BT mask = klib::bitfield::make_mask<Width, BT>();                                                        \
    return static_cast<Type>((m_data >> Offset) & mask);                                                               \
  }

#define BF_WO(Type, Name, Offset, Width)                                                                               \
  constexpr void set_##Name(Type value) noexcept {                                                                     \
    using BT = decltype(m_data);                                                                                       \
    static_assert(Offset + Width <= sizeof(BT) * 8, "Bitfield [" #Name "] exceeds storage width");                     \
    constexpr BT mask = klib::bitfield::make_mask<Width, BT>();                                                        \
    BT raw_val = static_cast<BT>(value);                                                                               \
    m_data = (m_data & ~(mask << Offset)) | ((raw_val & mask) << Offset);                                              \
  }                                                                                                                    \
  [[nodiscard]] constexpr auto with_##Name(Type value) const noexcept {                                                \
    auto copy = *this;                                                                                                 \
    copy.set_##Name(value);                                                                                            \
    return copy;                                                                                                       \
  }

// Bit Macros (single-bit booleans)
#define BF_BIT_RO(Name, Offset)                                                                                        \
  [[nodiscard]] constexpr bool get_##Name() const noexcept {                                                           \
    using BT = decltype(m_data);                                                                                       \
    static_assert(Offset < sizeof(BT) * 8, "Bit [" #Name "] exceeds storage width");                                   \
    return (m_data >> Offset) & BT{1};                                                                                 \
  }

#define BF_BIT_WO(Name, Offset)                                                                                        \
  constexpr void set_##Name(bool value) noexcept {                                                                     \
    using BT = decltype(m_data);                                                                                       \
    static_assert(Offset < sizeof(BT) * 8, "Bit [" #Name "] exceeds storage width");                                   \
    m_data = (m_data & ~(BT{1} << Offset)) | (static_cast<BT>(value) << Offset);                                       \
  }                                                                                                                    \
  constexpr void set_##Name() noexcept { set_##Name(true); }                                                           \
  constexpr void clear_##Name() noexcept { set_##Name(false); }                                                        \
  constexpr void toggle_##Name() noexcept { m_data ^= (decltype(m_data){1} << Offset); }                               \
  [[nodiscard]] constexpr auto with_##Name(bool value) const noexcept {                                                \
    auto copy = *this;                                                                                                 \
    copy.set_##Name(value);                                                                                            \
    return copy;                                                                                                       \
  }                                                                                                                    \
  [[nodiscard]] constexpr auto with_##Name() const noexcept { return with_##Name(true); }                              \
  [[nodiscard]] constexpr auto without_##Name() const noexcept { return with_##Name(false); }

#define BF_BIT_RW(Name, Offset)                                                                                        \
  BF_BIT_RO(Name, Offset)                                                                                              \
  BF_BIT_WO(Name, Offset)

// Write 1 to clear
#define BF_BIT_W1C(Name, Offset)                                                                                       \
  BF_BIT_RO(Name, Offset)                                                                                              \
  constexpr void mark_clear_##Name() noexcept {                                                                        \
    using BT = decltype(m_data);                                                                                       \
    static_assert(Offset < sizeof(BT) * 8, "Bit [" #Name "] exceeds storage width");                                   \
    m_data |= (BT{1} << Offset);                                                                                       \
  }                                                                                                                    \
  [[nodiscard]] constexpr auto with_mark_clear_##Name() const noexcept {                                               \
    auto copy = *this;                                                                                                 \
    copy.mark_clear_##Name();                                                                                          \
    return copy;                                                                                                       \
  }

#define BF_RW(Type, Name, Offset, Width)                                                                               \
  BF_RO(Type, Name, Offset, Width)                                                                                     \
  BF_WO(Type, Name, Offset, Width)

#define REG_RW(Name, Offset, Schema)                                                                                   \
  struct Name {                                                                                                        \
    static constexpr std::size_t offset = Offset;                                                                      \
    using Type = Schema;                                                                                               \
    static constexpr klib::bitfield::Access access = klib::bitfield::Access::RW;                                       \
  };

#define REG_RO(Name, Offset, Schema)                                                                                   \
  struct Name {                                                                                                        \
    static constexpr std::size_t offset = Offset;                                                                      \
    using Type = Schema;                                                                                               \
    static constexpr klib::bitfield::Access access = klib::bitfield::Access::RO;                                       \
  };

#define REG_WO(Name, Offset, Schema)                                                                                   \
  struct Name {                                                                                                        \
    static constexpr std::size_t offset = Offset;                                                                      \
    using Type = Schema;                                                                                               \
    static constexpr klib::bitfield::Access access = klib::bitfield::Access::WO;                                       \
  };

namespace klib::bitfield {
template <std::size_t Width, std::unsigned_integral T> consteval T make_mask() {
  if constexpr (Width >= sizeof(T) * 8) {
    return std::numeric_limits<T>::max();
  } else {
    return static_cast<T>((T{1} << Width) - 1);
  }
}

enum class Access { RW, RO, WO };

template <typename T>
concept ReadableReg = (T::access == Access::RW || T::access == Access::RO);

template <typename T>
concept WritableReg = (T::access == Access::RW || T::access == Access::WO);
} // namespace klib::bitfield
