#include <uacpi/kernel_api.h>

#include "../../include/boot/boot.hpp"
#include "boot/boot.hpp"
#include "utils/logger.hpp"

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
  *out_rsdp_address = reinterpret_cast<std::uintptr_t>(
                          kernel::boot::rsdp_request.response->address) -
                      kernel::boot::hhdm_request.response->offset;
  return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size) {
  return reinterpret_cast<void *>(addr +
                                  kernel::boot::hhdm_request.response->offset);
}

void uacpi_kernel_unmap(void *, uacpi_size) {}

void uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *str) {
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
