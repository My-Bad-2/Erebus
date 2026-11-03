/// @file boot.hpp
/// @brief The requests the kernel makes to Limine, and the check that they were answered.
///
/// Limine scans the image for these structures before jumping to `_start` and fills in their
/// `response` pointers. A request the bootloader could not satisfy keeps a null response, so
/// everything here except validate() is only meaningful once validate() has passed.
#pragma once

#include <cstdint>

#include "limine.h"

namespace kernel::boot {
/// @brief The Limine base revision the kernel is written against. Revisions change what some
///        responses mean, so validate() refuses a bootloader that does not support it.
inline constexpr std::uint64_t BASE_REVISION = 6;

/// @brief Where the ACPI RSDP is. Optional: without it there are no ACPI tables.
extern volatile limine_rsdp_request rsdp_request;

/// @brief Offset of the higher-half direct map. Required.
extern volatile limine_hhdm_request hhdm_request;

/// @brief The physical memory map. Required.
extern volatile limine_memmap_request memmap_request;

/// @brief The other CPUs, parked until the kernel wakes them. Optional: without it only the BSP
///        runs. Asks for x2APIC mode when the CPU has it.
extern volatile limine_mp_request mp_request;

/// @brief Where the kernel image was loaded, physically and virtually. Required.
extern volatile limine_executable_address_request executable_address_request;

/// @brief The kernel's ELF file as loaded from disk. Optional, and nothing reads it yet.
extern volatile limine_executable_file_request executable_file_request;

/// @brief Halts with a message unless Limine supports @ref BASE_REVISION and answered every
///        required request.
///
/// Called first thing in `_start`, so the rest of the kernel may dereference the required
/// responses without checking them.
void validate() noexcept;
}  // namespace kernel::boot
