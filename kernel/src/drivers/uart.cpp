#include "drivers/uart.hpp"
#include "hal/hal.hpp"

namespace kernel::drivers::uart {
std::expected<void, std::string_view> SerialPort::initialize(const std::uint32_t baud_rate,
                                                             std::uint32_t base_clock_hz) noexcept {
  // Probe for missing hardware or unmapped MMIO memory
  if (read<Lsr>().raw() == 0xff) {
    return std::unexpected("UART bus floats high (0xFF). Hardware missing or memory unmapped");
  }

  write<Ier>(IER{0});

  // 16550 samples 16x per bit.
  const auto divisor_32 = base_clock_hz / (16 * baud_rate);
  if (divisor_32 == 0 || divisor_32 > std::numeric_limits<std::uint16_t>::max()) {
    return std::unexpected("Baud rate unsupported by current base clock.");
  }

  const auto divisor = static_cast<std::uint16_t>(divisor_32);

  // Configure Baud Rate via DLAB
  write<Lcr>(LCR{0}.with_dlab());
  write<Dll>(divisor & 0xFFU);
  write<Dlm>(static_cast<std::uint8_t>(divisor >> std::uint8_t{8}));

  // Clear DLAB, set 8N1 (8 bits, No parity, 1 stop bit)
  write<Lcr>(LCR{}.with_word_length(3).without_dlab());

  // Attempt to enable 16750 64-byte FIFO (ignored by older hardware)
  const auto fcr = FCR{}
                       .with_fifo_enable()        // enable fifo
                       .with_clear_rx()           // clear receiver
                       .with_clear_tx()           // clear transmitter
                       .with_enable_64byte_fifo() // enable 64-byte fifo
                       .with_trigger_level(3);    // trigger lvl 3
  write<Fcr>(fcr);

  // Read back IIR to verify actual hardware capabilities
  const auto iir = read<Iir>();
  if (iir.get_fifo_status() == 3) {
    // Detect PCIe 16750 UART / 16550 UART
    m_fifo_depth = iir.get_fifo_64byte_enabled() != 0u ? 64 : 16;
  } else {
    m_fifo_depth = 1; // Legacy 16450 (No FIFO).
  }

  // Enable OUT2, DTR, and RTS
  write<Mcr>(MCR{}.with_dtr().with_rts().with_out2());
  return {};
}

void SerialPort::append(const std::string_view str) const noexcept {
  [[assume(m_fifo_depth == 1 || m_fifo_depth == 16 || m_fifo_depth == 64)]];

  std::uint8_t fifo_space_remaining = 0;

  auto push_char = [&](char c) {
    if (fifo_space_remaining == 0) {
      while (!is_transmit_fifo_empty()) {
        hw::cpu_relax();
      }

      fifo_space_remaining = m_fifo_depth;
    }

    write<THR>(static_cast<std::uint8_t>(c));
    --fifo_space_remaining;
  };

  for (const auto c : str) {
    if (c == '\n') {
      push_char('\r');
    }

    push_char(c);
  }
}

void SerialPort::push(const char c) const noexcept {
  if (c == '\n') {
    while (!is_transmit_fifo_empty()) {
      hw::cpu_relax();
    }

    write<THR>('\r');
  }

  while (!is_transmit_fifo_empty()) {
    hw::cpu_relax();
  }

  write<THR>(c);
}

std::expected<char, ReceiveError> SerialPort::read_char() const noexcept {
  while (!has_data()) {
    hw::cpu_relax();
  }

  const auto lsr = read<Lsr>();

  if (lsr.get_overrun_error()) {
    return std::unexpected(ReceiveError::Overrun);
  }

  if (lsr.get_parity_error()) {
    return std::unexpected(ReceiveError::Parity);
  }

  if (lsr.get_framing_error()) {
    return std::unexpected(ReceiveError::Framing);
  }

  if (lsr.get_break_interrupt()) {
    return std::unexpected(ReceiveError::BreakInterrupt);
  }

  return static_cast<char>(read<RBR>());
}
} // namespace kernel::drivers::uart
