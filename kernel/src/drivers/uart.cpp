#include "drivers/uart.hpp"

#include <limits>

#include "hal/hal.hpp"

namespace kernel::drivers::uart {
namespace {
constexpr std::uint32_t TX_WAIT_LIMIT = 100'000;
}  // namespace

bool SerialPort::probe() const noexcept {
  if (read<Lsr>().raw() == 0xFF) {
    return false;
  }

  write<Ier>(IER{0x00});
  const bool cleared = (read<Ier>().raw() & 0x0F) == 0x00;
  write<Ier>(IER{0x0F});
  const bool set = (read<Ier>().raw() & 0x0F) == 0x0F;
  write<Ier>(IER{0x00});

  return cleared && set;
}

std::expected<void, std::string_view> SerialPort::initialize(const std::uint32_t baud_rate,
                                                             std::uint32_t base_clock_hz) noexcept {
  write<Lcr>(read<Lcr>().without_dlab());

  if (!probe()) {
    return std::unexpected("no UART responds at this address");
  }

  if (baud_rate != 0) {
    if (base_clock_hz == 0) {
      base_clock_hz = DEFAULT_CLOCK_HZ;
    }

    // The UART samples every bit 16 times. Round to the nearest divisor.
    const std::uint64_t bit_clock = 16ull * baud_rate;
    const std::uint64_t divisor = (base_clock_hz + bit_clock / 2) / bit_clock;
    if (divisor == 0 || divisor > std::numeric_limits<std::uint16_t>::max()) {
      return std::unexpected("baud rate unsupported by the UART clock");
    }

    write<Lcr>(LCR{0}.with_dlab());
    write<Dll>(static_cast<std::uint8_t>(divisor & 0xFF));
    write<Dlm>(static_cast<std::uint8_t>(divisor >> 8));
  }

  // 8N1, DLAB off.
  write<Lcr>(LCR{}.with_word_length(3));

  // Ask for the 64-byte FIFO of a 16750; anything older ignores that bit.
  const auto fcr = FCR{}
                       .with_fifo_enable()
                       .with_clear_rx()
                       .with_clear_tx()
                       .with_enable_64byte_fifo()
                       .with_trigger_level(3);
  write<Fcr>(fcr);

  // IIR[7:6] == 3 means working FIFOs (16550A and later); a plain 16550 reports 2 because its
  // FIFO is broken, and a 16450 has none.
  const auto iir = read<Iir>();
  if (iir.get_fifo_status() == 3) {
    m_fifo_depth = iir.get_fifo_64byte_enabled() != 0 ? 64 : 16;
  } else {
    m_fifo_depth = 1;
  }

  write<Mcr>(MCR{}.with_dtr().with_rts().with_out2());

  m_tx_room = 0;
  m_stalled = false;
  return {};
}

bool SerialPort::wait_for_tx_room() noexcept {
  const std::uint32_t limit = m_stalled ? 1 : TX_WAIT_LIMIT;
  for (std::uint32_t i = 0; i < limit; ++i) {
    if (read<Lsr>().get_thr_empty()) {
      m_tx_room = m_fifo_depth;
      m_stalled = false;
      return true;
    }

    hw::pause();
  }

  m_stalled = true;
  return false;
}

void SerialPort::put(const std::uint8_t byte) noexcept {
  if (m_tx_room == 0 && !wait_for_tx_room()) [[unlikely]] {
    return;  // the transmitter is stuck; better to lose output than the machine
  }

  write<THR>(byte);
  --m_tx_room;
}

void SerialPort::push(const char c) noexcept {
  if (c == '\n') {
    put('\r');
  }

  put(static_cast<std::uint8_t>(c));
}

void SerialPort::append(const std::string_view str) noexcept {
  for (const char c : str) {
    push(c);
  }
}

void SerialPort::append_fill(const std::size_t count, const char c) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    push(c);
  }
}

void SerialPort::flush() const noexcept {
  for (std::uint32_t i = 0; i < TX_WAIT_LIMIT && !read<Lsr>().get_transmitter_empty(); ++i) {
    hw::pause();
  }
}

std::expected<char, ReceiveError> SerialPort::take(const LSR lsr) const noexcept {
  const auto byte = static_cast<char>(read<RBR>());

  if (lsr.get_break_interrupt()) return std::unexpected(ReceiveError::BreakInterrupt);
  if (lsr.get_framing_error()) return std::unexpected(ReceiveError::Framing);
  if (lsr.get_parity_error()) return std::unexpected(ReceiveError::Parity);
  if (lsr.get_overrun_error()) return std::unexpected(ReceiveError::Overrun);

  return byte;
}

std::expected<char, ReceiveError> SerialPort::read_char() const noexcept {
  LSR lsr = read<Lsr>();
  while (!lsr.get_data_ready()) {
    hw::cpu_relax();
    lsr = read<Lsr>();
  }

  return take(lsr);
}

std::expected<char, ReceiveError> SerialPort::try_read_char() const noexcept {
  const LSR lsr = read<Lsr>();
  if (!lsr.get_data_ready()) {
    return std::unexpected(ReceiveError::Empty);
  }

  return take(lsr);
}
}  // namespace kernel::drivers::uart
