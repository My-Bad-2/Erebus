#include <uacpi/kernel_api.h>

#include "boot/boot.hpp"
#include "utils/logger.hpp"

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address) {
  const auto* rsdp = kernel::boot::rsdp_request.response;
  if (rsdp == nullptr || rsdp->address == nullptr) {
    return UACPI_STATUS_NOT_FOUND;
  }

  // Some base revisions hand the RSDP over as a physical address, others (6 among them) as a
  // pointer into the HHDM. Anything below the HHDM is physical already.
  const auto addr = reinterpret_cast<std::uintptr_t>(rsdp->address);
  const std::uintptr_t hhdm = kernel::boot::hhdm_request.response->offset;
  *out_rsdp_address = addr >= hhdm ? addr - hhdm : addr;
  return UACPI_STATUS_OK;
}

// Tables are reached through the direct map, so mapping is a matter of adding the offset and
// unmapping has nothing to undo. This relies on the firmware keeping its tables in the ACPI
// regions of the memory map, which the direct map covers.
void* uacpi_kernel_map(const uacpi_phys_addr addr, uacpi_size) {
  return reinterpret_cast<void*>(addr + kernel::boot::hhdm_request.response->offset);
}

void uacpi_kernel_unmap(void*, uacpi_size) {}

// uACPI ends its messages with a newline already.
void uacpi_kernel_log(const uacpi_log_level lvl, const uacpi_char* str) {
  using namespace kernel::utils;

  switch (lvl) {
    case UACPI_LOG_ERROR:
      logger::error("{}", str);
      break;
    case UACPI_LOG_WARN:
      logger::warn("{}", str);
      break;
    case UACPI_LOG_INFO:
      logger::info("{}", str);
      break;
    case UACPI_LOG_TRACE:
      logger::trace("{}", str);
      break;
    case UACPI_LOG_DEBUG:
      logger::debug("{}", str);
      break;
  }
}
