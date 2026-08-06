#include "boot/boot.hpp"

namespace kernel::boot {
	volatile limine_rsdp_request rsdp_request = {
			.id = LIMINE_RSDP_REQUEST_ID,
			.revision = 0,
			.response = nullptr,
	};

	volatile limine_hhdm_request hhdm_request = {
			.id = LIMINE_HHDM_REQUEST_ID,
			.revision = 0,
			.response = nullptr,
	};
} // namespace kernel::boot
