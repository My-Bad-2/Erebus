/// Note: The kernel only uses bare minimum uACPI to provide ACPI table mappings. Rest features will
/// be implemented in userspace.

#include <uacpi/kernel_api.h>

#include "boot/boot.hpp"
#include "utils/logger.hpp"

using namespace kernel;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address) {
  const auto* rsdp = boot::rsdp_request.response;
  if (rsdp == nullptr || rsdp->address == nullptr) {
    return UACPI_STATUS_NOT_FOUND;
  }

  const auto addr = reinterpret_cast<std::uintptr_t>(rsdp->address);
  const std::uintptr_t hhdm = boot::hhdm_request.response->offset;
  *out_rsdp_address = addr >= hhdm ? addr - hhdm : addr;
  return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(const uacpi_phys_addr addr, uacpi_size) {
  return reinterpret_cast<void*>(addr + boot::hhdm_request.response->offset);
}

void uacpi_kernel_unmap(void*, uacpi_size) {}

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
