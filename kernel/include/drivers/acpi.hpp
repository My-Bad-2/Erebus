#ifndef EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_HPP
#define EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_HPP

#include "acpi/table.hpp"

namespace kernel::drivers::acpi {
	void early_initialize() noexcept;
} // namespace kernel::drivers::acpi

#endif // EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_HPP
