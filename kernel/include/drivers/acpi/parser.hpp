#pragma once

#include <type_traits>

namespace kernel::drivers::acpi {
using Status = uacpi_status;

template <typename T>
concept AcpiStruct = std::is_standard_layout_v<T> && std::is_trivial_v<T>;

template <std::size_t N> struct Signature {
  char data[5]{};

  consteval Signature(const char (&s)[N]) noexcept {
    static_assert(N == 5, "ACPI Signature must be exactly 4 characters.");

    for (int i = 0; i < 4; ++i) {
      data[i] = s[i];
    }
  }
};

} // namespace kernel::drivers::acpi
