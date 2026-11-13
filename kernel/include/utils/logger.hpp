#pragma once

#include <kformat.hpp>
#include <source_location>
#include <utility>

#include "hal/hal.hpp"
#include "lock.hpp"

namespace kernel::drivers::uart {
class SerialPort;
}

namespace kernel::utils::logger {
enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

constexpr auto ACTIVE_LOG_LEVEL = Level::Trace;

/// @brief Compound structure binding a compile-time format string to its
///        invocation site.
///
/// @tparam Args The types of the arguments to format.
template <typename... Args>
struct log_loc_str {
  klib::format_string<Args...> fmt;
  std::source_location loc;

  template <std::size_t N>
  consteval log_loc_str(const char (&s)[N],
                        const std::source_location l = std::source_location::current()) noexcept
      : fmt(s), loc(l) {}
};

namespace detail {
struct LogStyle {
  klib::color::fg f;
  klib::color::bg b;
  klib::color::style s;
};

constexpr LogStyle get_level_style(Level lvl) noexcept {
  using namespace klib;

  switch (lvl) {
    case Level::Trace:
      return {
          .f = color::fg::white,
          .b = color::bg::reset,
          .s = color::style::dim,
      };
    case Level::Debug:
      return {
          .f = color::fg::cyan,
          .b = color::bg::reset,
          .s = color::style::none,
      };
    case Level::Info:
      return {
          .f = color::fg::green,
          .b = color::bg::reset,
          .s = color::style::none,
      };
    case Level::Warn:
      return {
          .f = color::fg::yellow,
          .b = color::bg::reset,
          .s = color::style::bold,
      };
    case Level::Error:
      return {
          .f = color::fg::red,
          .b = color::bg::reset,
          .s = color::style::bold,
      };
    case Level::Fatal:
      return {
          .f = color::fg::white,
          .b = color::bg::red,
          .s = color::style::bold,
      };
    default:
      return {
          .f = color::fg::reset,
          .b = color::bg::reset,
          .s = color::style::none,
      };
  }
}

constexpr const char* level_name(Level lvl) noexcept {
  switch (lvl) {
    case Level::Trace:
      return "TRC";
    case Level::Debug:
      return "DBG";
    case Level::Info:
      return "INF";
    case Level::Warn:
      return "WRN";
    case Level::Error:
      return "ERR";
    case Level::Fatal:
      return "FTL";
    default:
      return "???";
  }
}

constexpr std::string_view basename(std::string_view path) noexcept {
  const auto idx = path.find_last_of("/\\");
  return (idx == std::string_view::npos) ? path : path.substr(idx + 1);
}
}  // namespace detail

/// @brief Moves console output to @p port, which must be initialised already.
void initialize(const drivers::uart::SerialPort& port) noexcept;

/// @brief Writes @p msg to the console as one piece. Safe from interrupt context.
void write_console(std::string_view msg) noexcept;

namespace detail {
// Where a line comes from takes this much room, whatever it is, so that messages line up: the
// file name in 14 columns, a colon, and the line in 4.
inline constexpr int FILE_WIDTH = 14;
inline constexpr int LINE_WIDTH = 4;
inline constexpr int ORIGIN_WIDTH = FILE_WIDTH + 1 + LINE_WIDTH;

// Every line starts with its level.
template <Level Lvl>
constexpr void begin_line(klib::StringBuffer& buf) noexcept {
  const auto [f, b, s] = get_level_style(Lvl);
  klib::format_to(buf, "[{}] ", klib::styled(level_name(Lvl), f, b, s));
}

// Every line ends in a newline, one that was cut short included: its last character gives way,
// or the next line would carry on from it on the console. Then it goes out in one piece.
inline void end_line(klib::StringBuffer& buf) noexcept {
  const std::span<char> active_span = buf.active_span();
  if (!active_span.empty() && active_span.back() != '\n') {
    if (const std::span<char> slot = buf.advance(1); !slot.empty()) {
      slot.front() = '\n';
    } else {
      active_span.back() = '\n';
    }
  }

  write_console(buf.view());
}
}  // namespace detail

template <Level Lvl, typename... Args>
constexpr void log(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  if constexpr (Lvl < ACTIVE_LOG_LEVEL) {
    return;
  }

  // Phase 1: Lock-free, concurrent, thread-local formatting.
  char local_buffer[512];
  klib::StringBuffer buf(local_buffer);

  detail::begin_line<Lvl>(buf);
  klib::format_to(buf, "{}:{} | ",
                  klib::dyn(detail::basename(fmt.loc.file_name()), detail::FILE_WIDTH),
                  klib::dyn(fmt.loc.line(), detail::LINE_WIDTH));
  klib::format_to(buf, fmt.fmt, std::forward<Args>(args)...);
  detail::end_line(buf);
}

/// @brief log(), with @p origin where the file and line would be, in the same width.
///
/// For a line that says what something other than kernel code has to say, user code through
/// DebugLog for one: the file and line of the kernel code that passes it on would only mislabel
/// it.
template <Level Lvl, typename... Args>
constexpr void log_from(const std::string_view origin,
                        klib::format_string<std::type_identity_t<Args>...> fmt,
                        Args&&... args) noexcept {
  if constexpr (Lvl < ACTIVE_LOG_LEVEL) {
    return;
  }

  char local_buffer[512];
  klib::StringBuffer buf(local_buffer);

  detail::begin_line<Lvl>(buf);
  klib::format_to(buf, "{} | ", klib::dyn(origin, detail::ORIGIN_WIDTH));
  klib::format_to(buf, fmt, std::forward<Args>(args)...);
  detail::end_line(buf);
}

template <typename... Args>
constexpr void info_from(const std::string_view origin,
                         klib::format_string<std::type_identity_t<Args>...> fmt,
                         Args&&... args) noexcept {
  log_from<Level::Info>(origin, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void trace(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Trace>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void debug(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Debug>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void info(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Info>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void warn(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Warn>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void error(log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Error>(fmt, std::forward<Args>(args)...);
}

/// @brief Logs the message and halts the machine: every other CPU is stopped, then this one.
template <typename... Args>
[[noreturn]] constexpr void fatal(log_loc_str<std::type_identity_t<Args>...> fmt,
                                  Args&&... args) noexcept {
  log<Level::Fatal>(fmt, std::forward<Args>(args)...);
  hw::panic_halt();
}
}  // namespace kernel::utils::logger