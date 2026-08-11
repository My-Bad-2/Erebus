#include "drivers/acpi.hpp"
#include "utils/logger.hpp"

#include <uacpi/uacpi.h>

#include <cstddef>

namespace kernel::drivers::acpi {
namespace {
// 8 kb early buffer enough to hold 75 tables
std::byte early_buf[8192];

void setup_spcr() {
  auto spcr_res = Table::find<"SPCR">();

  if (!spcr_res) [[unlikely]] {
    utils::logger::warn("Failed to find SPCR table.\n");
    return;
  }

  Table spcr_table = std::move(*spcr_res);
  auto spcr = spcr_table.as<acpi_spcr>();
  if (!spcr) [[unlikely]] {
    utils::logger::error("Malformed SPCR table.\n");
    return;
  }

  const acpi_spcr *table = *spcr;
  utils::logger::get_debug_console(table);
}
} // namespace

void early_initialize() noexcept {
  uacpi_setup_early_table_access(&early_buf, sizeof(early_buf));

  // Setup SPCR console if available
  setup_spcr();
}
} // namespace kernel::drivers::acpi
