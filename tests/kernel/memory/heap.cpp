#include "memory/heap.hpp"

#include <string.h>

#include <new>

#include "ktest.hpp"

namespace kernel::testing {
namespace {
using memory::heap::CacheFlags;
using memory::heap::KmemCache;

constexpr std::size_t OBJECTS = 3000;
void* g_ptrs[OBJECTS];
std::size_t g_sizes[OBJECTS];
void* g_cache_objects[1000];

// First and last byte of an object filled with its tag.
[[nodiscard]] bool intact(const void* p, const std::size_t size, const std::size_t tag) noexcept {
  const auto* b = static_cast<const std::uint8_t*>(p);
  return b[0] == static_cast<std::uint8_t>(tag) && b[size - 1] == static_cast<std::uint8_t>(tag);
}
}  // namespace

KTEST(Kmalloc, keeps_objects_apart_through_frees_and_reallocs) {
  Rng rng{0xC0FFEE};
  for (int round = 0; round < 6; ++round) {
    // Small objects from the caches in even rounds, large ones in odd rounds.
    for (std::size_t i = 0; i < OBJECTS; ++i) {
      g_sizes[i] = 1 + rng.below((round & 1) ? 40000 : 4096);
      g_ptrs[i] = memory::heap::kmalloc(g_sizes[i]);
      KREQUIRE_NE(g_ptrs[i], nullptr);
      klib::memset(g_ptrs[i], static_cast<int>(i & 0xFF), g_sizes[i]);
    }

    for (std::size_t i = 0; i < OBJECTS; i += 2) {
      KEXPECT(intact(g_ptrs[i], g_sizes[i], i));
      memory::heap::kfree(g_ptrs[i]);
    }

    for (std::size_t i = 1; i < OBJECTS; i += 2) {
      g_ptrs[i] = memory::heap::krealloc(g_ptrs[i], g_sizes[i] * 2);
      KREQUIRE_NE(g_ptrs[i], nullptr);
      KEXPECT(intact(g_ptrs[i], g_sizes[i], i));
      memory::heap::kfree(g_ptrs[i]);
    }
  }
}

KTEST(Kmalloc, honours_aligned_new) {
  for (int i = 0; i < 200; ++i) {
    void* p = ::operator new(100, std::align_val_t{256});
    KEXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 256, 0u);
    ::operator delete(p, std::align_val_t{256});
  }
}

KTEST(KmemCache, survives_red_zones_and_poison) {
  const auto cache = KmemCache::create(72, 8, CacheFlags::RedZone | CacheFlags::Poison);
  KREQUIRE(cache.has_value());

  for (int round = 0; round < 4; ++round) {
    for (void*& object : g_cache_objects) {
      const auto r = (*cache)->alloc();
      KREQUIRE(r.has_value());
      object = *r;
      klib::memset(object, 0x5A, 72);
    }

    for (void* object : g_cache_objects) {
      (*cache)->free(object);
    }
  }

  (*cache)->destroy();
}
}  // namespace kernel::testing
