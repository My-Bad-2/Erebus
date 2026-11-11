#include "boot/boot.hpp"
#include "utils/logger.hpp"

namespace kernel::boot {
namespace {
[[gnu::used, gnu::section(".limine_requests_start")]]
volatile std::uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

[[gnu::used, gnu::section(".limine_requests_end")]]
volatile std::uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

[[gnu::used, gnu::section(".limine_requests")]]
volatile std::uint64_t base_revision[] = LIMINE_BASE_REVISION(BASE_REVISION);
}  // namespace

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .flags = LIMINE_MP_REQUEST_X86_64_X2APIC,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_file_request executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

void validate() noexcept {
  if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) [[unlikely]] {
    utils::logger::fatal("Boot: the bootloader does not support Limine base revision {}.\n",
                         BASE_REVISION);
  }

  if (hhdm_request.response == nullptr) [[unlikely]] {
    utils::logger::fatal("Boot: Limine did not provide the higher-half direct map.\n");
  }

  const limine_memmap_response* memmap = memmap_request.response;
  if (memmap == nullptr || memmap->entry_count == 0 || memmap->entries == nullptr) [[unlikely]] {
    utils::logger::fatal("Boot: Limine did not provide a memory map.\n");
  }

  if (executable_address_request.response == nullptr) [[unlikely]] {
    utils::logger::fatal("Boot: Limine did not say where the kernel was loaded.\n");
  }
}
}  // namespace kernel::boot
