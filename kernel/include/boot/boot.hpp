#pragma once

#include <cstdint>

#include "limine.h"

namespace kernel::boot {
inline constexpr std::uint64_t BASE_REVISION = 6;

extern volatile limine_rsdp_request rsdp_request;
extern volatile limine_hhdm_request hhdm_request;
extern volatile limine_memmap_request memmap_request;
extern volatile limine_mp_request mp_request;
extern volatile limine_executable_address_request executable_address_request;
extern volatile limine_executable_file_request executable_file_request;
extern volatile limine_module_request module_request;

/// @brief Halts with a message unless Limine supports @ref BASE_REVISION and answered every
///        required request.
///
/// Called first thing in `_start`, so the rest of the kernel may dereference the required
/// responses without checking them.
void validate() noexcept;
}  // namespace kernel::boot
