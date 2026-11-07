#include "hpet.hpp"

#include <uacpi/tables.h>

#include "drivers/acpi.hpp"
#include "drivers/clock.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"

namespace kernel::drivers {
namespace {
constexpr std::size_t HPET_REGISTER_BYTES = 0x400;
}  // namespace

std::uint64_t SystemClock::initialize_hpet() noexcept {
  using namespace acpi;

  auto hpet_res = Table::find<ACPI_HPET_SIGNATURE>();
  if (!hpet_res) {
    utils::logger::info("No HPET table.\n");
    return 0;
  }

  const Table hpet_tbl = std::move(*hpet_res);
  const auto hpet_hdr_res = hpet_tbl.as<acpi_hpet>();
  if (!hpet_hdr_res) {
    utils::logger::warn("HPET table invalid: {}\n", hpet_hdr_res.error());
    return 0;
  }

  const acpi_gas& base_addr = (*hpet_hdr_res)->address;
  if (base_addr.address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_MEMORY || base_addr.address == 0) {
    utils::logger::warn("HPET is not in system memory (address space {}); ignoring it.\n",
                        base_addr.address_space_id);
    return 0;
  }

  const auto mapped = memory::vmm::map_device_registers(memory::PhysicalAddress{base_addr.address},
                                                        HPET_REGISTER_BYTES);
  if (!mapped) {
    utils::logger::warn("Could not map the HPET at {:#x}: {}\n", base_addr.address,
                        std::to_underlying(mapped.error()));
    return 0;
  }

  s_hpet.set_base(*mapped);

  const auto cap = hpet::GeneralCap{s_hpet.read<std::uint64_t>(hpet::GENERAL_CAP_AND_ID_OFFSET)};
  const std::uint8_t rev_id = cap.get_rev_id();
  const std::uint32_t period_fs = cap.get_counter_clk_period_fs();

  // IA-PC HPET 1.0a requires 0 < period <= 100 ns, i.e. 100,000,000 fs.
  if (rev_id == 0 || rev_id == 0xff || period_fs == 0 || period_fs > 100'000'000) {
    utils::logger::warn("HPET at {:#x} looks bogus (revision {}, period {} fs); ignoring it.\n",
                        base_addr.address, rev_id, period_fs);
    s_hpet.set_base(memory::VirtualAddress{});
    return 0;
  }

  s_hpet_mask = cap.get_counter_64bit_cap() ? std::numeric_limits<std::uint64_t>::max()
                                            : std::numeric_limits<std::uint32_t>::max();

  const auto cfg = hpet::GeneralConfig{s_hpet.read<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET)};
  if (!cfg.get_enable_cnf() || cfg.get_legacy_replacement_cnf()) {
    s_hpet.write<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET,
                                cfg.without_legacy_replacement_cnf().with_enable_cnf().raw());
  }

  return period_fs;
}

std::uint64_t SystemClock::read_hpet() noexcept {
  return s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET) & s_hpet_mask;
}

bool SystemClock::initialize_pm_timer() noexcept {
  acpi_fadt* fadt = nullptr;
  if (uacpi_table_fadt(&fadt) != UACPI_STATUS_OK || !fadt) {
    return false;
  }

  if (fadt->flags & ACPI_HW_REDUCED_ACPI) {
    return false;
  }

  const acpi_gas& blk = fadt->x_pm_tmr_blk;
  if (blk.address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_IO || blk.address == 0 ||
      blk.address > 0xFFFF || blk.register_bit_width < 24) {
    return false;
  }

  s_pm_timer_port = static_cast<std::uint16_t>(blk.address);
  s_pm_timer_mask = (fadt->flags & ACPI_TMR_VAL_EXT) ? 0xFFFF'FFFFul : 0xFF'FFFFul;
  return true;
}

std::uint64_t SystemClock::read_pm_timer() noexcept {
  return hw::IoResource{hw::IoType::PMIO, s_pm_timer_port}.read<std::uint32_t>(0) & s_pm_timer_mask;
}
}  // namespace kernel::drivers
