#include "boot/boot.hpp"

namespace kernel::boot {
[[gnu::section(".limine_requests_start")]] volatile std::uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;
[[gnu::section(".limine_requests_end")]] volatile std::uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;
[[gnu::section(".limine_requests")]] volatile std::uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

[[gnu::section(".limine_requests")]]
volatile limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::section(".limine_requests")]]
volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::section(".limine_requests")]]
volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::section(".limine_requests")]]
volatile limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .flags = LIMINE_MP_REQUEST_X86_64_X2APIC,
};
} // namespace kernel::boot
