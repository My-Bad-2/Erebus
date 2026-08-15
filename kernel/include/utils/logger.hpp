#pragma once

#include <kformat.hpp>
#include <source_location>
#include <utility>

#include "../drivers/uart.hpp"

namespace kernel::utils::logger {
enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

constexpr auto ACTIVE_LOG_LEVEL = Level::Trace;

template <typename... Args> struct log_loc_str {
  klib::format_string<Args...> fmt;
  std::source_location loc;

  template <std::size_t N>
  consteval log_loc_str(const char (&s)[N], const std::source_location l = std::source_location::current()) noexcept
      : fmt(s), loc(l) {}
};

namespace detail {
struct LogStyle {
  klib::fg::code f;
  klib::bg::code b;
  klib::text_style::code s;
};

constexpr LogStyle get_level_style(Level lvl) noexcept {
  using namespace klib;

  switch (lvl) {
  case Level::Trace:
    return {.f = fg::white, .b = bg::reset, .s = text_style::dim};
  case Level::Debug:
    return {.f = fg::cyan, .b = bg::reset, .s = text_style::none};
  case Level::Info:
    return {.f = fg::green, .b = bg::reset, .s = text_style::none};
  case Level::Warn:
    return {.f = fg::yellow, .b = bg::reset, .s = text_style::bold};
  case Level::Error:
    return {.f = fg::red, .b = bg::reset, .s = text_style::bold};
  case Level::Fatal:
    return {.f = fg::white, .b = bg::red, .s = text_style::bold};
  default:
    return {.f = fg::reset, .b = bg::reset, .s = text_style::none};
  }
}

constexpr const char *level_name(Level lvl) noexcept {
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

constexpr const char *basename(const char *path) noexcept {
  const char *file = path;
  while (*path) {
    if (*path == '/' || *path == '\\') {
      file = path + 1;
    }

    path++;
  }
  return file;
}
} // namespace detail

drivers::uart::SerialPort &get_debug_console(const void *spcr = nullptr) noexcept;

template <Level Lvl, typename... Args>
constexpr void log(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  if constexpr (Lvl < ACTIVE_LOG_LEVEL) {
    return;
  }

  auto &sink = get_debug_console();

  const auto [f, b, s] = detail::get_level_style(Lvl);
  const char *lvl_name = detail::level_name(Lvl);
  const char *file_name = detail::basename(fmt.loc.file_name());

  klib::format_to(sink, "[{}] {}:{} | ", klib::styled(lvl_name, f, b, s), klib::dyn(file_name, 14),
                  klib::dyn(fmt.loc.line(), 4));

  klib::format_to(sink, fmt.fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void trace(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Trace>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void debug(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Debug>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void info(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Info>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void warn(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Warn>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void error(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Error>(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
constexpr void fatal(log_loc_str<std::type_identity_t<Args>...> fmt, Args &&...args) noexcept {
  log<Level::Fatal>(fmt, std::forward<Args>(args)...);
  while (true)
    ;
}
} // namespace kernel::utils::logger
