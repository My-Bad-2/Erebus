#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "ctype.h"
#include "stdlib.h"

namespace klib {
namespace {
// The value of digit `c` in bases up to 36, or 255 if it is not one.
[[gnu::always_inline]] constexpr unsigned digit_value(const unsigned char c) noexcept {
  if (c - '0' + 0u < 10u) {
    return c - '0';
  }

  const unsigned letter = (c | 0x20u) - 'a';
  return letter < 26u ? letter + 10 : 255;
}

template <typename T>
constexpr T strtoX(const char* __restrict nptr, char** __restrict endptr, int base) noexcept {
  using U = std::make_unsigned_t<T>;
  constexpr bool is_signed = std::is_signed_v<T>;

  const char* s = nptr;

  while (isspace(*s)) {
    ++s;
  }

  bool neg = false;
  if (*s == '-') {
    neg = true;
    ++s;
  } else if (*s == '+') {
    ++s;
  }

  // "0x" counts as a prefix only when a hex digit follows; otherwise the '0' alone is the
  // number and parsing stops at the 'x'.
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 0x20) == 'x') {
    if (digit_value(static_cast<unsigned char>(s[2])) < 16) {
      base = 16;
      s += 2;
    } else if (base == 0) {
      base = 8;
    }
  } else if (base == 0) {
    if (*s == '0') {
      base = 8;
    } else {
      base = 10;
    }
  }

  if (base < 2 || base > 36) {
    if (endptr) {
      *endptr = const_cast<char*>(nptr);
    }

    return 0;
  }

  U limit;
  if constexpr (is_signed) {
    const U abs_min = static_cast<U>(std::numeric_limits<T>::max()) + 1;
    limit = neg ? abs_min : static_cast<U>(std::numeric_limits<T>::max());
  } else {
    limit = std::numeric_limits<T>::max();
  }

  const U cutoff = limit / base;
  const U cutlim = limit % base;

  U acc = 0;
  bool any_digits = false;
  bool overflow = false;

  while (true) {
    const unsigned val = digit_value(static_cast<unsigned char>(*s));
    if (std::cmp_greater_equal(val, base)) {
      break;
    }

    any_digits = true;

    if (overflow) {
      ++s;
      continue;
    }

    if (acc > cutoff || (acc == cutoff && val > cutlim)) {
      overflow = true;
    } else {
      acc = (acc * base) + val;
    }

    ++s;
  }

  if (!any_digits) {
    if (endptr) {
      *endptr = const_cast<char*>(nptr);
    }

    return 0;
  }

  if (endptr) {
    *endptr = const_cast<char*>(s);
  }

  if (overflow) {
    if constexpr (is_signed) {
      return neg ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    } else {
      return std::numeric_limits<T>::max();
    }
  }

  // Negate in the unsigned type: -LONG_MIN does not exist as a long. The conversion back is
  // modular, which turns the magnitude of LONG_MIN into LONG_MIN itself.
  return static_cast<T>(neg ? U{0} - acc : acc);
}
}  // namespace

long strtol(const char* __restrict str, char** __restrict endptr, int base) noexcept {
  return strtoX<long>(str, endptr, base);
}

long long strtoll(const char* __restrict str, char** __restrict endptr, int base) noexcept {
  return strtoX<long long>(str, endptr, base);
}

unsigned long strtoul(const char* __restrict str, char** __restrict endptr, int base) noexcept {
  return strtoX<unsigned long>(str, endptr, base);
}

unsigned long long strtoull(const char* __restrict str, char** __restrict endptr,
                            int base) noexcept {
  return strtoX<unsigned long long>(str, endptr, base);
}
}  // namespace klib
