#include "hal/fpu.hpp"

#include <string.h>

#include "fpu_registers.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "memory/heap.hpp"
#include "sched/fpu_guard.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
namespace fpu = hw::fpu;
namespace read = hw::read;
using hw::CpuInfo;
using hw::Feature;

// Room for one save area, and for noticing a save that does not stay inside it.
constexpr std::size_t TAIL_BYTES = 256;
constexpr std::uint8_t TAIL_PATTERN = 0x5A;

// Checked here because a mistake cannot get as far as a test: every CPU writes this to IA32_XFD on
// its way up, and on a CPU with AMX the bit of a component XFD does not cover is a #GP.
static_assert(fpu::XFD_NO_AMX.raw() == hw::XCR0::XTILEDATA);

// And what it cannot disable, every thread has to save.
static_assert((hw::XCR0::STANDARD_MASK & hw::XCR0::XTILECFG) != 0);

[[nodiscard]] const CpuInfo& cpu_info() noexcept { return *hw::percpu::self()->info; }

[[nodiscard]] bool uses_compacted_format() noexcept {
  return cpu_info().has<Feature::XSS>() || cpu_info().has<Feature::XSAVEC>();
}

// A zeroed, initialized save area with a marked tail, on the heap.
class TestArea {
  void* m_memory;

 public:
  TestArea() noexcept
      : m_memory(memory::heap::kmalloc(fpu::area_size() + TAIL_BYTES, fpu::AREA_ALIGNMENT)) {
    if (m_memory == nullptr) {
      return;
    }

    klib::memset(m_memory, 0, fpu::area_size());
    klib::memset(tail(), TAIL_PATTERN, TAIL_BYTES);
    fpu::init_area(m_memory, fpu::state_mask());
  }

  ~TestArea() noexcept { memory::heap::kfree(m_memory); }

  TestArea(const TestArea&) = delete;
  TestArea& operator=(const TestArea&) = delete;

  [[nodiscard]] void* get() const noexcept { return m_memory; }
  [[nodiscard]] std::uint8_t* tail() const noexcept {
    return static_cast<std::uint8_t*>(m_memory) + fpu::area_size();
  }

  [[nodiscard]] bool tail_is_intact() const noexcept {
    for (std::size_t i = 0; i < TAIL_BYTES; ++i) {
      if (tail()[i] != TAIL_PATTERN) {
        return false;
      }
    }

    return true;
  }
};

using SaveFn = void (*)(void*, std::uint64_t) noexcept;
using RestoreFn = void (*)(const void*, std::uint64_t) noexcept;

// Takes the registers to the area and back, with something else in them in between.
template <SaveFn save, RestoreFn restore>
void check_round_trip(const std::uint32_t cpu) noexcept {
  const TestArea area;
  KREQUIRE(area.get() != nullptr);

  const RegisterFile pattern = make_pattern(cpu + 1);
  RegisterFile restored{};
  {
    utils::NullLock lock;
    utils::FpuGuard guard{lock};

    load_registers(pattern);
    save(area.get(), fpu::state_mask());
    load_registers(make_pattern(cpu + 1000));
    restore(area.get(), fpu::state_mask());
    store_registers(restored);
  }

  KEXPECT(restored == pattern);
  KEXPECT(area.tail_is_intact());
}
}  // namespace

// First in this file on purpose: no test before it touches the FPU, and every one after it does.
KTEST_ALL_CPUS(Fpu, every_cpu_starts_with_the_default_controls, cpu) {
  RegisterFile found{};
  {
    utils::NullLock lock;
    utils::FpuGuard guard{lock};
    store_registers(found);
  }

  KEXPECT_EQ(found.fcw, fpu::DEFAULT_FCW);
  KEXPECT_EQ(found.mxcsr, fpu::DEFAULT_MXCSR);
}

KTEST_ALL_CPUS(Fpu, every_cpu_is_set_up_the_same, cpu) {
  const hw::CR0 cr0 = read::cr0();
  KEXPECT(!cr0.get_em());
  KEXPECT(!cr0.get_ts());
  KEXPECT(cr0.get_mp());
  KEXPECT(cr0.get_ne());

  const hw::CR4 cr4 = read::cr4();
  KEXPECT(cr4.get_osfxsr());
  KEXPECT(cr4.get_osxmmexcpt());
  KEXPECT(cr4.get_osxsave() == cpu_info().has<Feature::XSAVE>());
  KEXPECT(cr4.get_pke() == cpu_info().has<Feature::PKU>());

  if (cpu_info().has<Feature::XSAVE>()) {
    // No more than what the areas were sized for, on every CPU.
    KEXPECT_EQ(read::xcr0().raw(), fpu::state_mask(true));
  }

  if (fpu::amx_available()) {
    KEXPECT_EQ(read::msr(fpu::MSR_IA32_XFD), fpu::XFD_NO_AMX.raw());
  }
}

KTEST(Fpu, pkru_is_left_to_the_thread_switch) {
  KEXPECT_EQ(fpu::state_mask(true) & hw::XCR0{}.with_pkru().raw(), 0u);
}

// The static_assert above against the CPU itself, which only a CPU with AMX can answer.
KTEST(Fpu, xfd_disables_every_tile_component_it_can) {
  if (!fpu::amx_available()) {
    KTEST_SKIP("the CPU has no AMX");
  }

  constexpr std::uint32_t SUPPORTS_XFD = 1u << 2;

  for (const std::uint32_t component : {17u, 18u}) {
    const bool can_disable = (CpuInfo::query(0xD, component).ecx & SUPPORTS_XFD) != 0;
    const bool disabled = ((fpu::XFD_NO_AMX.raw() >> component) & 1) != 0;
    KEXPECT(disabled == can_disable);
  }
}

KTEST(Fpu, tile_data_only_comes_when_asked_for) {
  constexpr std::uint64_t TILES = hw::XCR0::XTILECFG | hw::XCR0::XTILEDATA;

  if (fpu::amx_available()) {
    KEXPECT_EQ(fpu::state_mask() & TILES, hw::XCR0::XTILECFG);
    KEXPECT_EQ(fpu::state_mask(true) & TILES, TILES);
    KEXPECT_GT(fpu::area_size(true), fpu::area_size() + 8000);
  } else {
    KEXPECT_EQ(fpu::state_mask() & TILES, 0u);
    KEXPECT_EQ(fpu::state_mask(true), fpu::state_mask());
    KEXPECT_EQ(fpu::area_size(true), fpu::area_size());
  }
}

// CPUID sizes everything in XCR0, tile state included, which is what area_size(true) holds. The
// compacted size can only be compared when nothing else is in it: CPUID also counts whatever is
// in IA32_XSS, and the areas leave that out.
KTEST(Fpu, area_size_agrees_with_cpuid) {
  if (!cpu_info().has<Feature::XSAVE>()) {
    KEXPECT_EQ(fpu::state_mask(), 0u);
    KEXPECT_EQ(fpu::area_size(), fpu::XSAVE_HEADER_OFFSET);
    return;
  }

  const std::size_t size = fpu::area_size(true);

  if (!uses_compacted_format()) {
    KEXPECT_EQ(size, CpuInfo::query(0xD, 0).ebx);
    return;
  }

  const bool has_supervisor_state =
      cpu_info().has<Feature::XSS>() && read::msr(fpu::MSR_IA32_XSS) != 0;
  if (has_supervisor_state) {
    KEXPECT_LE(size, CpuInfo::query(0xD, 1).ebx);
  } else {
    KEXPECT_EQ(size, CpuInfo::query(0xD, 1).ebx);
  }
}

KTEST(Fpu, init_area_writes_the_header_the_save_instruction_will) {
  if (!cpu_info().has<Feature::XSAVE>()) {
    KTEST_SKIP("FXSAVE areas have no header");
  }

  const TestArea area;
  KREQUIRE(area.get() != nullptr);

  const auto* header = reinterpret_cast<const fpu::XSaveHeader*>(
      static_cast<const std::byte*>(area.get()) + fpu::XSAVE_HEADER_OFFSET);

  // Anything in XCOMP_BV that is not enabled in XCR0 makes XRSTORS fault.
  const std::uint64_t expected =
      uses_compacted_format() ? (1ul << 63) | fpu::state_mask() : std::uint64_t{0};
  KEXPECT_EQ(header->xstate_bv, 0u);
  KEXPECT_EQ(header->xcomp_bv, expected);
}

// Goes by the registers rather than the area: this has to hold for every format there is.
KTEST(Fpu, fresh_area_restores_the_initial_state) {
  const TestArea area;
  KREQUIRE(area.get() != nullptr);

  RegisterFile restored{};
  {
    utils::NullLock lock;
    utils::FpuGuard guard{lock};

    load_registers(make_pattern(3));
    fpu::restores(area.get(), fpu::state_mask());
    store_registers(restored);
  }

  KEXPECT(restored == RegisterFile{});
}

KTEST_ALL_CPUS(Fpu, save_and_restore_round_trip, cpu) {
  check_round_trip<&fpu::save, &fpu::restore>(cpu);
}

KTEST_ALL_CPUS(Fpu, saves_and_restores_round_trip, cpu) {
  check_round_trip<&fpu::saves, &fpu::restores>(cpu);
}

// Saving twice in a row is what happens to a thread that does not touch the FPU in between, and
// is where XSAVEOPT and XSAVES start skipping what has not changed.
KTEST(Fpu, saving_again_changes_nothing) {
  const TestArea area;
  KREQUIRE(area.get() != nullptr);

  const RegisterFile pattern = make_pattern(9);
  RegisterFile restored{};
  {
    utils::NullLock lock;
    utils::FpuGuard guard{lock};

    load_registers(pattern);
    fpu::saves(area.get(), fpu::state_mask());
    fpu::restores(area.get(), fpu::state_mask());
    fpu::saves(area.get(), fpu::state_mask());
    load_registers(make_pattern(10));
    fpu::restores(area.get(), fpu::state_mask());
    store_registers(restored);
  }

  KEXPECT(restored == pattern);
}
}  // namespace kernel::testing
