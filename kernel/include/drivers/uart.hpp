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

class LCR {
  std::uint8_t m_data;

public:
  constexpr explicit LCR(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_RW(std::uint8_t, word_length, 0, 2)
  BF_BIT_RW(stop_bits, 2)
  BF_RW(std::uint8_t, parity, 3, 3)
  BF_BIT_RW(break_enable, 6)
  BF_BIT_RW(dlab, 7) // Divisor Latch Access Bit
};

class FCR {
  std::uint8_t m_data;

public:
  constexpr explicit FCR(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_BIT_WO(fifo_enable, 0)
  BF_BIT_WO(clear_rx, 1)
  BF_BIT_WO(clear_tx, 2)
  BF_BIT_WO(dma_mode, 3)
  BF_BIT_WO(enable_64byte_fifo, 5)
  BF_WO(std::uint8_t, trigger_level, 6, 2)
};

class IIR {
  std::uint8_t m_data;

public:
  constexpr explicit IIR(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_BIT_RO(interrupt_pending, 0)
  BF_RO(std::uint8_t, interrupt_id, 1, 3)
  BF_BIT_RO(fifo_64byte_enabled, 5)
  BF_RO(std::uint8_t, fifo_status, 6, 2)
};

class LSR {
  std::uint8_t m_data;

public:
  constexpr explicit LSR(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_BIT_RO(data_ready, 0)
  BF_BIT_RO(overrun_error, 1)
  BF_BIT_RO(parity_error, 2)
  BF_BIT_RO(framing_error, 3)
  BF_BIT_RO(break_interrupt, 4)
  BF_BIT_RO(thr_empty, 5)
  BF_BIT_RO(transmitter_empty, 6)
  BF_BIT_RO(fifo_error, 7)
};

class IER {
  std::uint8_t m_data;

public:
  constexpr explicit IER(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_BIT_RW(rx_available, 0)
  BF_BIT_RW(tx_empty, 1)
  BF_BIT_RW(line_status, 2)
  BF_BIT_RW(modem_status, 3)
};

class MCR {
  std::uint8_t m_data;

public:
  constexpr explicit MCR(std::uint8_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint8_t raw() const noexcept { return m_data; }

  BF_BIT_RW(dtr, 0)
  BF_BIT_RW(rts, 1)
  BF_BIT_RW(out1, 2)
  BF_BIT_RW(out2, 3)
  BF_BIT_RW(loopback, 4)
};

class SerialPort {
  hw::IoResource m_io;
  std::uint8_t m_fifo_depth{1};

  REG_WO(THR, 0, std::uint8_t) // Transmit Holding
  REG_RO(RBR, 0, std::uint8_t) // Receive Buffer

  // DLAB = 1
  REG_RW(Dll, 0, std::uint8_t) // Divisor Latch Low
  REG_RW(Dlm, 1, std::uint8_t) // Divisor Latch High

  REG_RW(Ier, 1, IER)
  REG_RO(Iir, 2, IIR)
  REG_WO(Fcr, 2, FCR)
  REG_RW(Lcr, 3, LCR)
  REG_RW(Mcr, 4, MCR)
  REG_RO(Lsr, 5, LSR)

  template <typename Reg>
    requires klib::bitfield::WritableReg<Reg>
  void write(Reg::Type val) const noexcept {
    static_assert(Reg::access != klib::bitfield::Access::RO, "Attempting to write a Read-Only register!");

    if constexpr (std::is_integral_v<typename Reg::Type>) {
      m_io.write<typename Reg::Type>(Reg::offset, val);
    } else {
      using BackingType = decltype(val.raw());
      m_io.write<BackingType>(Reg::offset, val.raw());
    }
  }

  template <typename Reg>
    requires klib::bitfield::ReadableReg<Reg>
  [[nodiscard, gnu::always_inline]] Reg::Type read() const noexcept {
    static_assert(Reg::access != klib::bitfield::Access::WO, "Attempting to read a Write-Only register!");

    if constexpr (std::is_integral_v<typename Reg::Type>) {
      return m_io.read<typename Reg::Type>(Reg::offset);
    } else {
      using BackingType = decltype(std::declval<typename Reg::Type>().raw());
      auto val = m_io.read<BackingType>(Reg::offset);
      return typename Reg::Type(val);
    }
  }

public:
  constexpr explicit SerialPort(const hw::IoResource io) noexcept : m_io(io) {}

  [[nodiscard]] std::expected<void, std::string_view> initialize(std::uint32_t baud_rate = 115200,
                                                                 std::uint32_t base_clock_hz = 1843200) noexcept;

  [[nodiscard]] bool is_transmit_fifo_empty() const noexcept { return read<Lsr>().get_thr_empty() != 0; }
  [[nodiscard]] bool has_data() const noexcept { return read<Lsr>().get_data_ready() != 0; }

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
