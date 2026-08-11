#pragma once

#include "acpi/table.hpp"

namespace kernel::drivers::acpi {
void early_initialize() noexcept;
} // namespace kernel::drivers::acpi
