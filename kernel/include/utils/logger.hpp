#pragma once

#include <kformat.hpp>
#include <source_location>
#include <utility>

#include "drivers/uart.hpp"
#include "hal/hal.hpp"
#include "lock.hpp"

namespace kernel::utils::logger {
enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

constexpr auto ACTIVE_LOG_LEVEL = Level::Trace;

/**
 * @brief Compound structure binding a compile-time format string to its
 * invocation site.
 *
 * @tparam Args The types of the arguments to format.
 */
template <typename... Args>
struct log_loc_str {
  klib::format_string<Args...> fmt;
  std::source_location loc;

  template <std::size_t N>
  consteval log_loc_str(
      const char (&s)[N],
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

void initialize(const void* spcr = nullptr) noexcept;
void write_console(std::string_view msg) noexcept;

template <Level Lvl, typename... Args>
constexpr void log(log_loc_str<std::type_identity_t<Args>...> fmt,
                   Args&&... args) noexcept {
  if constexpr (Lvl < ACTIVE_LOG_LEVEL) {
    return;
  }

  // Phase 1: Lock-free, concurrent, thread-local formatting.
  char local_buffer[512];
  klib::StringBuffer buf(local_buffer);

  const auto [f, b, s] = detail::get_level_style(Lvl);
  const char* lvl_name = detail::level_name(Lvl);
  const auto file_name = detail::basename(fmt.loc.file_name());

  klib::format_to(buf, "[{}] {}:{} | ", klib::styled(lvl_name, f, b, s),
                  klib::dyn(file_name, 14), klib::dyn(fmt.loc.line(), 4));

  klib::format_to(buf, fmt.fmt, std::forward<Args>(args)...);

  // Ensure the payload is implicitly newline-terminated.
  std::span<char> active_span = buf.active_span();
  if (!active_span.empty() && active_span.back() != '\n') {
    buf.push('\n');
  }

  write_console(buf.view());
}

template <typename... Args>
constexpr void trace(log_loc_str<std::type_identity_t<Args>...> fmt,
                     Args&&... args) noexcept {
  log<Level::Trace>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void debug(log_loc_str<std::type_identity_t<Args>...> fmt,
                     Args&&... args) noexcept {
  log<Level::Debug>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void info(log_loc_str<std::type_identity_t<Args>...> fmt,
                    Args&&... args) noexcept {
  log<Level::Info>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void warn(log_loc_str<std::type_identity_t<Args>...> fmt,
                    Args&&... args) noexcept {
  log<Level::Warn>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void error(log_loc_str<std::type_identity_t<Args>...> fmt,
                     Args&&... args) noexcept {
  log<Level::Error>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
[[noreturn]] constexpr void fatal(
    log_loc_str<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
  log<Level::Fatal>(fmt, std::forward<Args>(args)...);
  hw::dead_loop();
}
}  // namespace kernel::utils::logger