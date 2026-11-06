#include "bitfield.hpp"

#include <cstdint>

#include "ktest.hpp"

namespace {
enum class Mode : std::uint8_t { Off = 0, Slow = 1, Fast = 2, Turbo = 3 };

class Control {
  std::uint32_t m_data;

 public:
  constexpr explicit Control(const std::uint32_t raw = 0) noexcept : m_data(raw) {}
  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return m_data; }

  BF_BIT_RW(enable, 0)
  BF_BIT_RO(busy, 1)
  BF_BIT_W1C(error, 2)
  BF_RW(Mode, mode, 4, 2)
  BF_RW(std::uint32_t, address, 12, 20)
  BF_RO(std::uint8_t, version, 8, 4)
};

class Byte {
  std::uint8_t m_data;

 public:
  constexpr explicit Byte(const std::uint8_t raw = 0) noexcept : m_data(raw) {}
  [[nodiscard]] constexpr std::uint8_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, low, 0, 4)
  BF_BIT_RW(top, 7)
};

class Full {
  std::uint64_t m_data;

 public:
  constexpr explicit Full(const std::uint64_t raw = 0) noexcept : m_data(raw) {}
  BF_RW(std::uint64_t, all, 0, 64)
};
}  // namespace

KTEST(Bitfield, chained_builders) {
  constexpr auto ctl = Control{}.with_enable().with_mode(Mode::Fast).with_address(0xABC12);
  static_assert(ctl.get_enable() && ctl.get_mode() == Mode::Fast);

  KEXPECT_EQ(ctl.raw(), 1u | (2u << 4) | (0xABC12u << 12));
  KEXPECT_EQ(ctl.get_address(), 0xABC12u);
  KEXPECT(!(ctl.without_enable().get_enable()));
}

KTEST(Bitfield, getters_read_their_bits_only) {
  const Control ctl{0xFFFF'F7FFu};
  KEXPECT(ctl.get_busy());
  KEXPECT_EQ(ctl.get_version(), 0x7u);
  KEXPECT_EQ(ctl.get_mode(), Mode::Turbo);
}

KTEST(Bitfield, setters_mask_oversized_values) {
  Control ctl{};
  ctl.set_address(0xFFFF'FFFFu);
  KEXPECT_EQ(ctl.get_address(), 0xF'FFFFu);
  KEXPECT_EQ(ctl.raw(), 0xFFFF'F000u);

  ctl.set_address(0);
  KEXPECT_EQ(ctl.raw(), 0u);
}

KTEST(Bitfield, single_bit_helpers) {
  Control ctl{};
  ctl.set_enable();
  KEXPECT(ctl.get_enable());
  ctl.toggle_enable();
  KEXPECT(!(ctl.get_enable()));
  ctl.set_enable(true);
  ctl.clear_enable();
  KEXPECT_EQ(ctl.raw(), 0u);
}

KTEST(Bitfield, write_one_to_clear) {
  const Control status{1u << 2};
  KEXPECT(status.get_error());

  // Acknowledging writes the 1 back.
  KEXPECT_EQ(Control{}.with_mark_clear_error().raw(), 1u << 2);
}

KTEST(Bitfield, narrow_storage) {
  Byte b{};
  b.set_low(0x1F);
  b.set_top();
  KEXPECT_EQ(b.raw(), 0x8Fu);
  KEXPECT_EQ(b.get_low(), 0xFu);
  b.clear_top();
  KEXPECT_EQ(b.raw(), 0x0Fu);
}

KTEST(Bitfield, full_width_field) {
  const auto f = Full{}.with_all(0xFFFF'FFFF'FFFF'FFFFull);
  KEXPECT_EQ(f.get_all(), 0xFFFF'FFFF'FFFF'FFFFull);
}

KTEST(Bitfield, make_mask) {
  static_assert(klib::bitfield::make_mask<0, std::uint8_t>() == 0);
  static_assert(klib::bitfield::make_mask<3, std::uint8_t>() == 0b111);
  static_assert(klib::bitfield::make_mask<8, std::uint8_t>() == 0xFF);
  static_assert(klib::bitfield::make_mask<64, std::uint64_t>() == ~0ul);
  KEXPECT(true);
}
