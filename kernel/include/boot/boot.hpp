#pragma once

#include "limine.h"

namespace kernel::boot {
extern volatile limine_rsdp_request rsdp_request;
extern volatile limine_hhdm_request hhdm_request;
extern volatile limine_memmap_request memmap_request;
extern volatile limine_mp_request mp_request;
extern volatile limine_executable_address_request executable_address_request;
extern volatile limine_executable_file_request executable_file_request;
} // namespace kernel::boot
