#include "hpet.hpp"

#include "drivers/acpi.hpp"
#include "drivers/clock.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"

namespace kernel::drivers {
std::uint64_t SystemClock::initialize_hpet() noexcept {
  using namespace acpi;
  auto hpet_res = Table::find<ACPI_HPET_SIGNATURE>();
  if (!hpet_res) utils::logger::fatal("HPET Table missing: {}\n", hpet_res.error());

  const Table hpet_tbl = std::move(*hpet_res);
  const auto hpet_hdr_res = hpet_tbl.as<acpi_hpet>();
  if (!hpet_hdr_res) utils::logger::fatal("HPET Table invalid: {}\n", hpet_hdr_res.error());

  const acpi_hpet* hpet = *hpet_hdr_res;
  const acpi_gas& base_addr = hpet->address;

  if (base_addr.address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_MEMORY)
    utils::logger::fatal("Invalid address space. Expected System memory.\n");

  memory::PhysicalAddress hpet_phys_addr{base_addr.address};

  memory::vmm::AddressSpace* kspace = memory::vmm::kernel_space();
  auto res = kspace->map_mmio(hpet_phys_addr, memory::PAGE_SIZE);
  if (!res) utils::logger::fatal("Could not map hpet: {}\n", res.error());

  memory::VirtualAddress hpet_vaddr = *res;
  s_hpet.set_base(hpet_vaddr);

  const auto cap = hpet::GeneralCap{s_hpet.read<std::uint64_t>(hpet::GENERAL_CAP_AND_ID_OFFSET)};
  const std::uint8_t rev_id = cap.get_rev_id();
  const std::uint32_t period_fs = cap.get_counter_clk_period_fs();

  // Guard against unpopulated MMIO and spec violations.
  // IA-PC HPET Spec 1.0a requires 0 < period_fs <= 100,000,00 fs (100 ns)
  if (rev_id == 0 || rev_id == 0xff || period_fs == 0 || period_fs > 100'000'000) {
    s_hpet.set_base(memory::VirtualAddress{});
    return 0;
  }

  const auto cfg = hpet::GeneralConfig{s_hpet.read<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET)};
  if (!cfg.get_enable_cnf() || cfg.get_legacy_replacement_cnf())
    s_hpet.write<std::uint64_t>(hpet::GENERAL_CONFIG_OFFSET,
                                cfg.without_legacy_replacement_cnf().with_enable_cnf().raw());

  return period_fs;
}

std::uint64_t SystemClock::try_calibrate_with_hpet(std::uint64_t period_fs) noexcept {
  // 10 ms window per rial: 10^13 / period_fs
  constexpr std::uint64_t WINDOW_FS = 10'000'000'000'000ul;
  const std::uint64_t target_hpet_ticks = std::max(1ul, WINDOW_FS / period_fs);

  std::array<std::uint64_t, 3> samples{};

  for (std::size_t i = 0; i < samples.size(); ++i) {
    // Align to an HPET counter tick edge
    const std::uint64_t hpet_initial = s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET);
    std::uint64_t hpet_start = hpet_initial;
    while (hpet_start == hpet_initial)
      hpet_start = s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET);

    const std::uint64_t tsc_start = hw::read_tsc();

    std::uint64_t hpet_end = hpet_start;
    while ((hpet_end - hpet_start) < target_hpet_ticks) {
      hw::cpu_relax();
      hpet_end = s_hpet.read<std::uint64_t>(hpet::MAIN_COUNTER_OFFSET);
    }

    const std::uint64_t tsc_end = hw::read_tsc();

    const std::uint64_t tsc_delta = tsc_end - tsc_start;
    const std::uint64_t hpet_delta = hpet_end - hpet_start;

    constexpr __uint128_t FS_PER_SEC = 1'000'000'000'000'000ul;
    const __uint128_t numer = static_cast<__uint128_t>(tsc_delta) * FS_PER_SEC;
    const __uint128_t denom = static_cast<__uint128_t>(hpet_delta) * period_fs;

    samples[i] = static_cast<std::uint64_t>(numer / denom);
  }

  std::ranges::sort(samples);
  s_calib_src = hw::timer::Source::HpetSoftware;
  return samples[1];  // Median of 3 samples
}
}  // namespace kernel::drivers