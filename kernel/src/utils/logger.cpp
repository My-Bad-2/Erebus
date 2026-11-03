#include "utils/logger.hpp"

#include "drivers/uart.hpp"
#include "utils/assert.hpp"

namespace kernel::utils::logger {
namespace {
std::optional<drivers::uart::SerialPort> console;
TicketSpinlock console_lock;

// Interrupt handlers log too, so the console lock must be taken with interrupts off, or one that
// arrives while its own CPU is printing deadlocks. IrqSaveGuard is no good here: it also counts
// preemption through GS, and the first messages are printed before GS is set up.
class [[nodiscard]] ConsoleGuard {
  std::uint64_t m_flags;

 public:
  ConsoleGuard() noexcept : m_flags(hw::irq::save()) { console_lock.lock(); }

  ~ConsoleGuard() noexcept {
    console_lock.unlock();
    hw::irq::restore(m_flags);
  }

  ConsoleGuard(const ConsoleGuard&) = delete;
  ConsoleGuard& operator=(const ConsoleGuard&) = delete;
};

// Until the firmware's console is known, try COM1. If nothing answers there, stay silent rather
// than spin on a port that is not there.
bool console_probed = false;

void ensure_console_init() noexcept {
  if (console_probed) [[likely]] {
    return;
  }

  console_probed = true;
  drivers::uart::SerialPort com1(hw::IoResource(hw::IoType::PMIO, 0x3f8, 1));
  if (com1.initialize(115200)) {
    console.emplace(com1);
    console->append("\x1b[2J");  // Clear screen on host (can be safely removed)
  }
}
}  // namespace

void initialize(const drivers::uart::SerialPort& port) noexcept {
  ConsoleGuard guard;

  // Whatever COM1 still has queued may be cut off once nobody writes to it; let it drain.
  if (console) {
    console->flush();
  }

  console_probed = true;
  console.emplace(port);
  console->append("[CONSOLE] Switched to the ACPI SPCR console.\n");
}

void write_console(const std::string_view msg) noexcept {
  ConsoleGuard guard;
  ensure_console_init();

  if (console) {
    console->append(msg);
  }
}
}  // namespace kernel::utils::logger

namespace kernel::utils {
void assertion_failed(const char* condition, const std::source_location where) noexcept {
  logger::fatal("Assertion failed: {} ({}:{} in {})\n", condition, where.file_name(), where.line(),
                where.function_name());
}
}  // namespace kernel::utils
