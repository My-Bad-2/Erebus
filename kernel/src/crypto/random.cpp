#include "crypto/random.hpp"

#include <algorithm>
#include <array>
#include <atomic>

#include "boot/boot.hpp"
#include "crypto/blake2b.hpp"
#include "crypto/bytes.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "hal/smp/cpu_info.hpp"
#include "hal/smp/percpu.hpp"
#include "utils/assert.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::crypto::random {
namespace {
using namespace klib::literals;

constexpr klib::Duration RESEED_INTERVAL = 60_s;

// Larger requests get a key of their own and are generated with interrupts enabled.
constexpr std::size_t LARGE_REQUEST_BYTES = 256;
constexpr int HW_RETRIES = 10;

struct HardwareRng {
  bool rdseed = false;
  bool rdrand = false;
};

constinit HardwareRng g_hw{};

[[nodiscard]] bool rdseed64(std::uint64_t& out) noexcept {
  for (int i = 0; i < HW_RETRIES; ++i) {
    bool ok = false;
    asm volatile("rdseed %0" : "=r"(out), "=@ccc"(ok));
    if (ok) {
      return true;
    }

    asm volatile("pause");
  }

  return false;
}

[[nodiscard]] bool rdrand64(std::uint64_t& out) noexcept {
  for (int i = 0; i < HW_RETRIES; ++i) {
    bool ok = false;
    asm volatile("rdrand %0" : "=r"(out), "=@ccc"(ok));
    if (ok) {
      return true;
    }
  }

  return false;
}

// Some CPUs report success while returning a constant: several AMD families hand out all ones
// after a suspend, and others have shipped microcode that returned zero. Real output never
// repeats a 64-bit word within a handful of reads.
template <typename Read>
[[nodiscard]] bool looks_random(Read read) noexcept {
  std::uint64_t prev = 0;
  for (int i = 0; i < 8; ++i) {
    std::uint64_t value = 0;
    if (!read(value) || value == 0 || value == ~std::uint64_t{0} || (i != 0 && value == prev)) {
      return false;
    }

    prev = value;
  }

  return true;
}

void detect_hardware() noexcept {
  using hw::CpuInfo;
  const std::uint32_t max_leaf = CpuInfo::query(0).eax;
  const bool has_rdrand = (CpuInfo::query(1).ecx >> 30) & 1;
  const bool has_rdseed = max_leaf >= 7 && ((CpuInfo::query(7, 0).ebx >> 18) & 1);

  g_hw.rdrand = has_rdrand && looks_random(rdrand64);
  g_hw.rdseed = has_rdseed && looks_random(rdseed64);

  if (has_rdrand && !g_hw.rdrand) {
    utils::logger::warn("Random: RDRAND returns constant values; not using it.\n");
  }

  if (has_rdseed && !g_hw.rdseed) {
    utils::logger::warn("Random: RDSEED returns constant values; not using it.\n");
  }
}

// Fills `words` from RDSEED, or RDRAND where RDSEED has nothing; words neither can fill stay as
// they were. Returns how many were filled.
std::size_t read_hardware(const std::span<std::uint64_t> words) noexcept {
  std::size_t filled = 0;
  for (std::uint64_t& word : words) {
    std::uint64_t value = 0;
    if ((g_hw.rdseed && rdseed64(value)) || (g_hw.rdrand && rdrand64(value))) {
      word ^= value;
      ++filled;
    }
  }

  return filled;
}

// Everything ever mixed in, compressed into a BLAKE2b state.
[[clang::no_destroy]] constinit Blake2b g_pool{};
constinit utils::QSpinlock g_pool_lock;

template <typename T>
void absorb(Blake2b& pool, const T& value) noexcept {
  pool.update(std::as_bytes(std::span{&value, 1}));
}

// Timings of short loops whose length depends on the previous timing. Each sample carries a
// little entropy from cache misses, branch prediction and interrupts; however little, the hash
// cannot lose what is there.
void absorb_jitter(Blake2b& pool) noexcept {
  std::array<std::uint64_t, 64> samples{};
  std::array<std::uint8_t, 256> scratch{};

  for (int round = 0; round < 4; ++round) {
    for (std::uint64_t& sample : samples) {
      const std::uint64_t start = hw::read_tsc();
      std::uint64_t x = start;
      for (std::uint64_t i = 0; i < 16 + (start & 15); ++i) {
        x = x * 6364136223846793005ull + scratch[x >> 56];
        scratch[x & 0xFF] = static_cast<std::uint8_t>(scratch[x & 0xFF] + 1);
      }

      sample = (hw::read_tsc() - start) ^ x;
    }

    absorb(pool, samples);
  }

  secure_zero(samples);
}

// Little of this is secret, but it differs between machines, and some of it between boots.
void absorb_boot_data(Blake2b& pool) noexcept {
  if (const limine_memmap_response* memmap = boot::memmap_request.response) {
    for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
      absorb(pool, memmap->entries[i]->base);
      absorb(pool, memmap->entries[i]->length);
      absorb(pool, memmap->entries[i]->type);
    }
  }

  if (const limine_executable_address_response* exec = boot::executable_address_request.response) {
    absorb(pool, exec->physical_base);
    absorb(pool, exec->virtual_base);
  }

  if (const limine_hhdm_response* hhdm = boot::hhdm_request.response) {
    absorb(pool, hhdm->offset);
  }

  if (const limine_rsdp_response* rsdp = boot::rsdp_request.response) {
    absorb(pool, rsdp->address);
  }

  for (const std::uint32_t leaf : {0u, 1u, 0x8000'0002u, 0x8000'0003u, 0x8000'0004u}) {
    absorb(pool, hw::CpuInfo::query(leaf));
  }

  absorb(pool, hw::read_tsc());
}

// A key from the pool. The pool then carries on keyed with the other half of the same digest,
// so neither the key and the pool states before it, nor those after, reveal one another.
void extract_key(chacha20::Key& key) noexcept {
  std::array<std::byte, Blake2b::MAX_DIGEST_BYTES> digest;
  {
    utils::IrqSaveGuard guard(g_pool_lock);
    Blake2b snapshot = g_pool;
    snapshot.finalize(digest);
    g_pool = Blake2b{Blake2b::MAX_DIGEST_BYTES, std::span{digest}.first<32>()};
  }

  std::ranges::copy(std::span{digest}.last<chacha20::KEY_BYTES>(), key.begin());
  secure_zero(digest);
}

constinit ChaChaRng g_base{};
constinit utils::QSpinlock g_base_lock;

// Bumped on every reseed; CPUs whose key is from an older generation fetch a new one. Zero
// until the first seed.
std::atomic<std::uint64_t> g_generation{0};

// When the next reseed is due, in clock nanoseconds. The clock reads 0 until it is calibrated,
// which postpones reseeding until then.
std::atomic<std::uint64_t> g_next_reseed_ns{0};

void reseed() noexcept {
  // Fresh hardware output first, so that a reseed never repeats a key even if nothing else
  // reached the pool in the meantime.
  std::array<std::uint64_t, 4> fresh{};
  fresh[0] = hw::read_tsc();
  (void)read_hardware(fresh);
  add_entropy(std::as_bytes(std::span{fresh}));
  secure_zero(fresh);

  chacha20::Key key;
  extract_key(key);
  {
    utils::IrqSaveGuard guard(g_base_lock);
    g_base.reseed(key);
    g_generation.fetch_add(1, std::memory_order_release);
  }

  secure_zero(key);
}

void maybe_reseed() noexcept {
  const std::uint64_t now = drivers::SystemClock::now().raw_ns();
  std::uint64_t due = g_next_reseed_ns.load(std::memory_order_relaxed);
  if (now < due) [[likely]] {
    return;
  }

  // Whoever moves the deadline does the reseed; the others carry on with the current key.
  const auto next = now + static_cast<std::uint64_t>(RESEED_INTERVAL.to_ns());
  if (g_next_reseed_ns.compare_exchange_strong(due, next, std::memory_order_relaxed)) {
    reseed();
  }
}
}  // namespace

void initialize() noexcept {
  if (g_generation.load(std::memory_order_acquire) != 0) {
    return;
  }

  detect_hardware();

  std::array<std::uint64_t, 16> hardware{};
  const std::size_t hardware_words = read_hardware(hardware);
  {
    utils::IrqSaveGuard guard(g_pool_lock);
    absorb(g_pool, hardware);
    absorb_jitter(g_pool);
    absorb_boot_data(g_pool);
  }

  secure_zero(hardware);
  reseed();
  g_next_reseed_ns.store(static_cast<std::uint64_t>(RESEED_INTERVAL.to_ns()),
                         std::memory_order_relaxed);

  if (hardware_words != 0) {
    utils::logger::info("Random: seeded with {} bits from {}, timing jitter and boot data.\n",
                        hardware_words * 64, g_hw.rdseed ? "RDSEED" : "RDRAND");
  } else {
    utils::logger::warn(
        "Random: no usable hardware RNG; seeded from timing jitter and boot data only.\n");
  }
}

void add_entropy(const std::span<const std::byte> data) noexcept {
  utils::IrqSaveGuard guard(g_pool_lock);
  g_pool.update(data);
}

void fill(const std::span<std::byte> out) noexcept {
  if (out.empty()) {
    return;
  }

  if (g_generation.load(std::memory_order_acquire) == 0) [[unlikely]] {
    initialize();
  }

  if (out.size() > LARGE_REQUEST_BYTES) {
    chacha20::Key key;
    fill(key);
    chacha20::keystream(key, 0, chacha20::Nonce{}, out);
    secure_zero(key);
    return;
  }

  maybe_reseed();

  // An interrupt handler on this CPU may want random bytes too.
  utils::IrqDisableGuard irq_off;
  CpuRandomState& cpu = hw::percpu::self()->random;

  const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
  if (cpu.generation != generation) [[unlikely]] {
    chacha20::Key key;
    {
      utils::IrqSaveGuard guard(g_base_lock);
      g_base.fill(key);
    }

    cpu.generator.reseed(key);
    cpu.generation = generation;
    secure_zero(key);
  }

  cpu.generator.fill(out);
}

std::uint64_t below(const std::uint64_t bound) noexcept {
  KASSERT(bound != 0);

  // Lemire's method: the high half of x * bound is uniform in [0, bound) once the few values of
  // x whose low half falls under 2^64 mod bound are drawn again.
  auto product = static_cast<unsigned __int128>(get<std::uint64_t>()) * bound;
  auto low = static_cast<std::uint64_t>(product);
  if (low < bound) [[unlikely]] {
    const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
    while (low < threshold) {
      product = static_cast<unsigned __int128>(get<std::uint64_t>()) * bound;
      low = static_cast<std::uint64_t>(product);
    }
  }

  return static_cast<std::uint64_t>(product >> 64);
}

bool has_hardware_seed() noexcept { return g_hw.rdseed || g_hw.rdrand; }
}  // namespace kernel::crypto::random
