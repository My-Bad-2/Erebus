#ifndef EREBUS_KERNEL_INCLUDE_BOOT_BOOT_HPP
#define EREBUS_KERNEL_INCLUDE_BOOT_BOOT_HPP

#include "limine.h"

namespace kernel::boot {
	extern volatile limine_rsdp_request rsdp_request;
	extern volatile limine_hhdm_request hhdm_request;
} // namespace kernel::boot

#endif // EREBUS_KERNEL_INCLUDE_BOOT_BOOT_HPP
