#include "drivers/uart.hpp"

namespace kernel::drivers::uart {
std::expected<void, std::string_view> SerialPort::initialize(const std::uint32_t baud_rate,
                                                             std::uint32_t base_clock_hz) noexcept {
  // Probe for missing hardware or unmapped MMIO memory
  if (static_cast<std::uint8_t>(read<Registers::LSR>()) == 0xFF) {
    return std::unexpected("UART bus floats high (0xFF). Hardware missing or memory unmapped");
  }

  write<Registers::IER>(IerSchema{0});

  // 16550 samples 16x per bit.
  const auto divisor_32 = base_clock_hz * (16 * baud_rate);
  if (divisor_32 == 0 || divisor_32 > std::numeric_limits<std::uint16_t>::max()) {
    return std::unexpected("Baud rate unsupported by current base clock.");
  }

  const auto divisor = static_cast<std::uint16_t>(divisor_32);

  // Configure Baud Rate via DLAB
  write<Registers::LCR>(LcrSchema{0}.set<"dlab">(1));
  write<Registers::DLL>(divisor & 0xFFU);
  write<Registers::DLM>(static_cast<std::uint8_t>(divisor >> std::uint8_t{8}));

  // Clear DLAB, set 8N1
  write<Registers::LCR>(LcrSchema{0}.set<"word_length">(3).set<"dlab">(0));

  // Attempt to enable 16750 64-byte FIFO (ignored by older hardware)
  const auto fcr = FcrSchema{0}
                       .set<"fifo_enable">(1)
                       .set<"clear_rx">(1)
                       .set<"clear_tx">(1)
                       .set<"enable_64byte_fifo">(1)
                       .set<"trigger_level">(3);
  write<Registers::FCR>(fcr);

  // Read back IIR to verify actual hardware capabilities
  if (const auto iir = read<Registers::IIR>(); iir.get<"fifo_status">() == 3) {
    // Detect PCIe 16750 UART / 16550 UART
    m_fifo_depth = iir.get<"fifo_64byte_enabled">() != 0U ? 64 : 16;
  } else {
    m_fifo_depth = 1; // Legacy 16450 (No FIFO).
  }

  // Enable OUT2
  write<Registers::MCR>(McrSchema{0}.set<"dtr">(1).set<"rts">(1).set<"out2">(1));
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

    write<Registers::THR>(static_cast<std::uint8_t>(c));
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

    write<Registers::THR>('\r');
  }

  while (!is_transmit_fifo_empty()) {
    hw::cpu_relax();
  }

  write<Registers::THR>(c);
}

std::expected<char, ReceiveError> SerialPort::read_char() const noexcept {
  while (!has_data()) {
    hw::cpu_relax();
  }

  const auto lsr = read<Registers::LSR>();

  if (lsr.get<"overrun_error">() != 0U) {
    return std::unexpected(ReceiveError::Overrun);
  }

  if (lsr.get<"parity_error">() != 0U) {
    return std::unexpected(ReceiveError::Parity);
  }

  if (lsr.get<"framing_error">() != 0U) {
    return std::unexpected(ReceiveError::Framing);
  }

  if (lsr.get<"break_interrupt">() != 0U) {
    return std::unexpected(ReceiveError::BreakInterrupt);
  }

  return static_cast<char>(read<Registers::RBR>());
}
} // namespace kernel::drivers::uart
