#pragma once

#include "hal/io.hpp"
#include <bitfield.hpp>
#include <expected>

namespace kernel::drivers::uart {
enum class ReceiveError : std::uint8_t {
  Overrun,        // Data lost because the driver didn't read it fast enough
  Parity,         // Hardware detected electrical noise/corruption
  Framing,        // Start/Stop bits misaligned (baud rate mismatch)
  BreakInterrupt, // Remote device intentionally held the line low
};

using LcrSchema = klib::BitfieldSchema<klib::Field<"word_length", 0, 2>, klib::Bit<"stop_bits", 2>,
                                       klib::Field<"parity", 3, 3>, klib::Bit<"break_enable", 6>, klib::Bit<"dlab", 7>>;

using FcrSchema = klib::BitfieldSchema<klib::Bit<"fifo_enable", 0>, klib::Bit<"clear_rx", 1>, klib::Bit<"clear_tx", 2>,
                                       klib::Bit<"dma_mode", 3>, klib::Reserved<4>, klib::Bit<"enable_64byte_fifo", 5>,
                                       klib::Field<"trigger_level", 6, 2>>;

using IirSchema = klib::BitfieldSchema<klib::ReadOnly<"interrupt_pending", 0, 1>, klib::ReadOnly<"interrupt_id", 1, 3>,
                                       klib::Reserved<4, 1>, klib::ReadOnly<"fifo_64byte_enabled", 5, 1>,
                                       klib::ReadOnly<"fifo_status", 6, 2>>;
using LsrSchema = klib::BitfieldSchema<klib::ReadOnly<"data_ready", 0, 1>, klib::ReadOnly<"overrun_error", 1, 1>,
                                       klib::ReadOnly<"parity_error", 2, 1>, klib::ReadOnly<"framing_error", 3, 1>,
                                       klib::ReadOnly<"break_interrupt", 4, 1>, klib::ReadOnly<"thr_empty", 5, 1>,
                                       klib::ReadOnly<"transmitter_empty", 6, 1>, klib::ReadOnly<"fifo_error", 7, 1>>;

using IerSchema = klib::BitfieldSchema<klib::Bit<"rx_available", 0>, klib::Bit<"tx_empty", 1>,
                                       klib::Bit<"line_status", 2>, klib::Bit<"modem_status", 3>, klib::Reserved<4, 4>>;

using McrSchema = klib::BitfieldSchema<klib::Bit<"dtr", 0>, klib::Bit<"rts", 1>, klib::Bit<"out1", 2>,
                                       klib::Bit<"out2", 3>, klib::Bit<"loopback", 4>, klib::Reserved<5, 3>>;

struct Registers {
  // DLAB = 0
  using THR = klib::Register<0, std::uint8_t, klib::Access::WO>;
  using RBR = klib::Register<0, std::uint8_t, klib::Access::RO>;

  // DLAB = 1
  using DLL = klib::Register<0, std::uint8_t, klib::Access::RW>;
  using DLM = klib::Register<1, std::uint8_t, klib::Access::RW>;

  // Standard
  using IER = klib::Register<1, IerSchema, klib::Access::RW>;
  using IIR = klib::Register<2, IirSchema, klib::Access::RO>;
  using FCR = klib::Register<2, FcrSchema, klib::Access::WO>;
  using LCR = klib::Register<3, LcrSchema, klib::Access::RW>;
  using MCR = klib::Register<4, McrSchema, klib::Access::RW>;
  using LSR = klib::Register<5, LsrSchema, klib::Access::RO>;
};

class SerialPort {
  hw::IoResource m_io;
  std::uint8_t m_fifo_depth{1};

  template <typename Reg> [[gnu::always_inline]] void write(Reg::ValueType val) const noexcept {
    static_assert(Reg::access != klib::Access::RO, "Attempting to write a Read-Only register!");
    m_io.write<typename Reg::HwType>(Reg::offset, static_cast<Reg::HwType>(val));
  }

  template <typename Reg> [[nodiscard, gnu::always_inline]] Reg::ValueType read() const noexcept {
    static_assert(Reg::access != klib::Access::RW, "Attempting to write a Write-Only register!");
    auto val = m_io.read<typename Reg::HwType>(Reg::offset);
    return static_cast<Reg::ValueType>(val);
  }

public:
  constexpr explicit SerialPort(const hw::IoResource io) noexcept : m_io(io) {}

  [[nodiscard]] std::expected<void, std::string_view> initialize(std::uint32_t baud_rate = 115200,
                                                                 std::uint32_t base_clock_hz = 1843200) noexcept;

  [[nodiscard]] bool is_transmit_fifo_empty() const noexcept { return read<Registers::LSR>().get<"thr_empty">() != 0; }

  [[nodiscard]] bool has_data() const noexcept { return read<Registers::LSR>().get<"data_ready">() != 0; }

  void append(std::string_view str) const noexcept;
  void push(char c) const noexcept;

  [[nodiscard]] static constexpr char *advance(std::size_t /* n */) noexcept {
    // UARTs are stream devices, not memory buffers. Formatter must fall back
    // to `push()` and `append()`.
    return nullptr;
  }

  [[nodiscard]] std::expected<char, ReceiveError> read_char() const noexcept;
  [[nodiscard]] std::expected<char, ReceiveError> try_read_char() const noexcept {
    if (!has_data()) {
      return '\0';
    }

    return read_char();
  }
};
} // namespace kernel::drivers::uart
