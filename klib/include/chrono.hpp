#pragma once

#include <compare>
#include <cstdint>

namespace klib {
class Duration {
  std::int64_t m_ns{0};

 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(std::int64_t nanoseconds) noexcept : m_ns(nanoseconds) {}

  static constexpr Duration nanoseconds(std::int64_t n) noexcept { return Duration(n); }
  static constexpr Duration microseconds(std::int64_t us) noexcept {
    return Duration{us * 1'000ll};
  }

  static constexpr Duration milliseconds(std::int64_t ms) noexcept {
    return Duration{ms * 1'000'000ll};
  }

  static constexpr Duration seconds(std::int64_t s) noexcept {
    return Duration{s * 1'000'000'000ll};
  }

  constexpr std::int64_t count() const noexcept { return m_ns; }
  constexpr std::int64_t to_ns() const noexcept { return m_ns; }
  constexpr std::int64_t to_us() const noexcept { return m_ns / 1'000LL; }
  constexpr std::int64_t to_ms() const noexcept { return m_ns / 1'000'000LL; }
  constexpr std::int64_t to_secs() const noexcept { return m_ns / 1'000'000'000LL; }

  constexpr Duration operator+() const noexcept { return *this; }
  constexpr Duration operator-() const noexcept { return Duration{-m_ns}; }

  constexpr Duration operator+(Duration rhs) const noexcept { return Duration{m_ns + rhs.m_ns}; }
  constexpr Duration operator-(Duration rhs) const noexcept { return Duration{m_ns - rhs.m_ns}; }

  template <std::integral Rep>
  constexpr Duration operator*(Rep factor) const noexcept {
    return Duration{m_ns * static_cast<std::int64_t>(factor)};
  }

  template <std::integral Rep>
  constexpr Duration operator/(Rep divisor) const noexcept {
    return Duration{m_ns / static_cast<std::int64_t>(divisor)};
  }

  constexpr Duration& operator+=(Duration rhs) noexcept {
    m_ns += rhs.m_ns;
    return *this;
  }

  constexpr Duration& operator-=(Duration rhs) noexcept {
    m_ns -= rhs.m_ns;
    return *this;
  }

  constexpr auto operator<=>(const Duration&) const noexcept = default;
};

class TimePoint {
  std::uint64_t m_ns_since_boot{0};

 public:
  constexpr TimePoint() = default;
  constexpr explicit TimePoint(std::uint64_t ns_since_boot) noexcept
      : m_ns_since_boot(ns_since_boot) {}

  constexpr std::uint64_t raw_ns() const noexcept { return m_ns_since_boot; }

  constexpr Duration time_since_epoch() const noexcept {
    return Duration{static_cast<std::int64_t>(m_ns_since_boot)};
  }

  constexpr TimePoint operator+(Duration d) const noexcept {
    return TimePoint{m_ns_since_boot + static_cast<std::uint64_t>(d.to_ns())};
  }

  constexpr TimePoint operator-(Duration d) const noexcept {
    return TimePoint{m_ns_since_boot - static_cast<std::uint64_t>(d.to_ns())};
  }

  constexpr Duration operator-(TimePoint rhs) const noexcept {
    return Duration{static_cast<std::int64_t>(m_ns_since_boot - rhs.m_ns_since_boot)};
  }

  constexpr TimePoint& operator+=(Duration d) noexcept {
    m_ns_since_boot += static_cast<std::uint64_t>(d.to_ns());
    return *this;
  }

  constexpr TimePoint& operator-=(Duration d) noexcept {
    m_ns_since_boot -= static_cast<std::uint64_t>(d.to_ns());
    return *this;
  }

  constexpr auto operator<=>(const TimePoint&) const noexcept = default;
};

namespace literals {
[[nodiscard]] constexpr Duration operator""_ns(unsigned long long n) noexcept {
  return Duration::nanoseconds(n);
}

[[nodiscard]] constexpr Duration operator""_us(unsigned long long us) noexcept {
  return Duration::microseconds(us);
}

[[nodiscard]] constexpr Duration operator""_ms(unsigned long long ms) noexcept {
  return Duration::milliseconds(ms);
}

[[nodiscard]] constexpr Duration operator""_s(unsigned long long s) noexcept {
  return Duration::seconds(s);
}
}  // namespace literals
}  // namespace klib