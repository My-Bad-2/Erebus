#include <string.h>

#include <atomic>

#include "ktest.hpp"
#include "memory/heap.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::testing {
namespace {
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;

// Objects allocated on one CPU and freed on another.
constexpr std::size_t SLOTS = 2048;
std::atomic<void*> g_slots[SLOTS];
std::atomic<bool> g_stop{false};
VirtualAddress g_lazy{};
constexpr std::size_t LAZY_PAGES = 4096;

// Each object is filled with its size, which also sits in its first word.
void put_object(Rng& rng) noexcept {
  const auto size = static_cast<std::uint32_t>(8 + rng.below(3000));
  void* p = memory::heap::kmalloc(size);
  KEXPECT_NE(p, nullptr);
  if (p == nullptr) {
    return;
  }

  klib::memset(p, static_cast<std::uint8_t>(size), size);
  *static_cast<std::uint32_t*>(p) = size;

  if (void* prev = g_slots[rng.below(SLOTS)].exchange(p, std::memory_order_acq_rel)) {
    memory::heap::kfree(prev);
  }
}

void take_object(Rng& rng) noexcept {
  void* p = g_slots[rng.below(SLOTS)].exchange(nullptr, std::memory_order_acq_rel);
  if (p == nullptr) {
    return;
  }

  const auto size = *static_cast<std::uint32_t*>(p);
  const auto* bytes = static_cast<const std::uint8_t*>(p);
  bool intact = true;
  for (std::size_t b = 4; b < size; b += 61) {
    intact = intact && bytes[b] == static_cast<std::uint8_t>(size);
  }
  KEXPECT(intact);

  memory::heap::kfree(p);
}
}  // namespace

// CPU 0 allocates, frees and takes demand faults on a lazy area (shared address space lock);
// the others allocate and free too, and now and then create and destroy a VMA (exclusive lock,
// and a TLB shootdown to every CPU when it is unmapped).
KTEST_ALL_CPUS(MemoryStress, survives_every_cpu_allocating_and_unmapping, cpu) {
  memory::vmm::AddressSpace* ks = memory::vmm::kernel_space();

  if (cpu == 0) {
    const auto lazy = ks->alloc(LAZY_PAGES * PAGE_SIZE, RW);
    KEXPECT(lazy.has_value());
    g_lazy = lazy.value_or(VirtualAddress{});
    g_stop.store(false, std::memory_order_relaxed);
  }
  sync_cpus();
  KREQUIRE(static_cast<bool>(g_lazy));

  Rng rng{0xA5A5A5A5ull * (cpu + 1)};
  if (cpu == 0) {
    for (std::uint64_t round = 0; round < 200000; ++round) {
      take_object(rng);
      put_object(rng);

      if (round % 32 == 0) {
        const std::size_t page = (round / 32) % LAZY_PAGES;
        auto* w = (g_lazy + page * PAGE_SIZE).as<volatile std::uint64_t>();
        *w = page;
        KEXPECT_EQ(*w, page);
      }
    }

    g_stop.store(true, std::memory_order_release);
  } else {
    while (!g_stop.load(std::memory_order_acquire)) {
      take_object(rng);
      if (rng.next() & 1) {
        put_object(rng);
      }

      if (rng.below(64) == 0) {
        const auto v = ks->alloc(2 * PAGE_SIZE, RW | AccessFlags::Populate);
        KEXPECT(v.has_value());
        if (v) {
          const std::uint64_t mark = rng.next();
          v->as<volatile std::uint64_t>()[0] = mark;
          KEXPECT_EQ(v->as<volatile std::uint64_t>()[0], mark);
          KEXPECT(ks->free(*v).has_value());
        }
      }
    }
  }
  sync_cpus();

  if (cpu == 0) {
    for (auto& slot : g_slots) {
      if (void* p = slot.exchange(nullptr)) {
        memory::heap::kfree(p);
      }
    }

    KEXPECT(ks->free(g_lazy).has_value());
    g_lazy = VirtualAddress{};
  }
}
}  // namespace kernel::testing
