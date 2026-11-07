/// @file heap.hpp
/// @brief SLUB-style object caches and the general purpose kernel heap.
///
/// A KmemCache manages objects of one size carved out of *slabs*: naturally aligned blocks of
/// 2^order pages from the PMM. The design follows Linux's SLUB closely:
///
///  - Every CPU owns at most one *active* slab and allocates from it without atomics, first from
///    a private free list of objects it freed itself, then by bumping through never-used memory.
///  - Objects freed by other CPUs are pushed onto the slab's lock-free *remote* free list (a CAS
///    on the packed heap::SlabState in the slab's head descriptor). The owner claims that whole
///    list in one go when it runs dry.
///  - A slab owned by a CPU is *frozen*. When the CPU moves on, the slab goes to a short per-CPU
///    partial list, to its node's partial list, back to the PMM if it is empty, or nowhere at
///    all if every object is in use. A remote free that turns a full, unfrozen slab partial
///    puts it on the node list; one that empties it may give it back to the PMM.
///
/// The accounting invariant behind all of this is
/// `in_use == total - (objects on the remote free list)`. CPU-owned free objects count as in use
/// until the CPU deactivates the slab, which is why claiming the remote list resets `in_use` to
/// `total`.
///
/// kmalloc() rounds requests up to a power of two between 8 bytes and 32 KiB and serves them
/// from one cache per size. Anything larger goes straight to the PMM. kfree() works out which of
/// the two it is from the frame descriptor: every frame of a slab records its cache.
///
/// Slabs nobody uses are not given back at once: a node keeps a few empty ones for the next
/// burst, and each CPU a few partial ones. Under memory pressure the reclaim daemon wants those
/// back, which is what shrink_node() is for. Every cache is on one list for it, so a shrink can
/// get to all of them; see memory/shrinker.hpp for how the daemon gets to the heap.

#pragma once

#include <string.h>

#include <cstdint>
#include <expected>
#include <new>
#include <span>

#include "core/errors.hpp"
#include "pmm/page.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::heap {
/// @brief Debugging aids for a cache. Both cost time on every allocation and free.
enum class CacheFlags : std::uint8_t {
  None = 0,
  RedZone = 1 << 0,  ///< Guard word after every object, checked on free (catches overruns).
  Poison = 1 << 1,   ///< Fill freed objects with a pattern, checked on alloc (catches UAF).
};

constexpr CacheFlags operator|(const CacheFlags a, const CacheFlags b) {
  return static_cast<CacheFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr CacheFlags operator&(const CacheFlags a, const CacheFlags b) {
  return static_cast<CacheFlags>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr CacheFlags operator~(const CacheFlags a) {
  return static_cast<CacheFlags>(~static_cast<std::uint8_t>(a));
}

constexpr bool has_flag(const CacheFlags flags, const CacheFlags flag) {
  return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(flag)) != 0;
}

/// @brief Per-CPU allocation state of one cache. Only ever touched by its CPU with IRQs off.
struct alignas(std::hardware_destructive_interference_size) CpuCache {
  void* local_freelist{nullptr};  ///< Objects of `page` freed by this CPU.
  char* bump_ptr{nullptr};        ///< Next never-used object of `page`.
  std::uint16_t bump_left{0};     ///< Never-used objects left after `bump_ptr`.

  std::uint8_t partial_count{0};
  pmm::Page* page{nullptr};         ///< Active (frozen) slab.
  pmm::Page* cpu_partial{nullptr};  ///< Frozen slabs with remote frees, linked by next_partial.

  static constexpr std::uint8_t MAX_CPU_PARTIAL = 4;

  /// @brief Lock-free fast path: local free list first, then the bump region.
  /// @return nullptr when both are exhausted and the slow path has to run.
  [[nodiscard]] void* alloc(std::uint32_t object_size) noexcept;
};

/// @brief Per-node list of partially used slabs that no CPU owns.
struct alignas(std::hardware_destructive_interference_size) NodeCache {
  utils::QSpinlock lock;
  std::uint32_t num_partial{0};
  pmm::Page* partial_head{nullptr};  ///< Mostly empty slabs, the best ones to allocate from.
  pmm::Page* partial_tail{nullptr};  ///< Mostly full slabs.

  /// Empty slabs are kept around (instead of going back to the PMM) until the list is this long.
  static constexpr std::uint32_t MIN_PARTIAL_PAGES = 8;
};

/// @brief A cache of fixed-size objects. See the file comment for how it works.
class alignas(std::hardware_destructive_interference_size) KmemCache {
  std::span<CpuCache> m_cpu_caches;
  std::span<NodeCache> m_node_caches;
  KmemCache* m_next_cache{nullptr};  ///< The next one on the list of every cache.

  void (*m_ctor)(void*);

  std::uint32_t m_obj_size;  ///< Size the user asked for.
  std::uint32_t m_size;      ///< Stride between objects: obj_size + red zone, aligned.
  std::uint8_t m_order;      ///< Slab size is PAGE_SIZE << m_order.
  CacheFlags m_flags;

  static constexpr std::uint8_t POISON_FREE_BYTE = 0x6B;
  static constexpr std::uint64_t REDZONE_MAGIC = 0xDEADBEEFCAFEBABE;

  [[nodiscard, gnu::cold, gnu::noinline]] std::expected<void*, Error> refill(
      CpuCache& cpu) noexcept;
  [[nodiscard]] void* claim_remote_objects(CpuCache& cpu, pmm::Page* page) noexcept;
  [[nodiscard]] std::expected<pmm::Page*, Error> new_slab() noexcept;
  void deactivate_slab(CpuCache& cpu) noexcept;
  void release_slab(pmm::Page* page) const noexcept;

  void publish_partial(pmm::Page* page) noexcept;
  void discard_if_empty(pmm::Page* page) noexcept;
  [[nodiscard]] pmm::Page* pop_from_node_partial(std::uint32_t node_id) noexcept;
  static void unlink_partial_locked(NodeCache& node, pmm::Page* slab) noexcept;

  // What shrink_node() is made of. The flushes run on every CPU of the node, with interrupts off.
  void flush_cpu_partials_local() noexcept;
  static void flush_cpu_partials_of(void* cache) noexcept;
  static void flush_every_cpu_partial(void*) noexcept;
  void unfreeze(pmm::Page* slab) noexcept;
  [[nodiscard]] std::size_t trim_node(std::uint32_t node_id) noexcept;

  static void add_to_cache_list(KmemCache* cache) noexcept;

  friend std::size_t shrink_node(std::uint32_t node_id) noexcept;
  friend void initialize() noexcept;

  [[nodiscard]] pmm::Page* slab_of(const void* obj) const noexcept;
  [[nodiscard]] std::uint16_t index_of(const pmm::Page* slab, const void* obj) const noexcept;
  [[nodiscard]] void* object_at(const pmm::Page* slab, std::uint16_t idx) const noexcept;

 public:
  constexpr KmemCache(const std::uint32_t obj_size, const std::uint32_t size,
                      const std::uint8_t order, const CacheFlags flags, void (*ctor)(void*),
                      const std::span<CpuCache> cpus, const std::span<NodeCache> nodes) noexcept
      : m_cpu_caches(cpus),
        m_node_caches(nodes),
        m_ctor(ctor),
        m_obj_size(obj_size),
        m_size(size),
        m_order(order),
        m_flags(flags) {}

  KmemCache(const KmemCache&) = delete;
  KmemCache& operator=(const KmemCache&) = delete;

  /// @brief Creates a cache.
  ///
  /// After boot, with interrupts on: a shrink holds the list of caches while it waits for other
  /// CPUs, which would wait for this one if it were spinning for the list with interrupts off.
  /// @param obj_size  Object size in bytes (> 0).
  /// @param alignment Object alignment, a power of two. At least pointer alignment is used.
  /// @param flags     Debugging aids. Poison is ignored for caches with a constructor, since the
  ///                  constructed state has to survive a free/alloc cycle.
  /// @param ctor      Optional constructor, run once per object when its slab is created, not
  ///                  on every alloc(). Objects must be returned to that state before free().
  [[nodiscard]] static std::expected<KmemCache*, Error> create(
      std::uint32_t obj_size, std::uint32_t alignment, CacheFlags flags = CacheFlags::None,
      void (*ctor)(void*) = nullptr) noexcept;

  /// @brief Returns every slab to the PMM and frees the cache itself.
  /// @pre No CPU is using the cache any more. Objects still allocated are leaked (and reported).
  ///      Interrupts are on, as for create().
  void destroy() noexcept;

  /// @brief Gives every slab of NUMA node @p node_id that has no objects in use back to the PMM.
  ///
  /// The CPUs of the node first let go of the partial slabs they keep for themselves, which takes
  /// an IPI. Their active slabs stay where they are: those are in use, or about to be.
  /// @pre The calling thread can wait for other CPUs, and the cache stays alive meanwhile.
  /// @return How many pages went back.
  std::size_t shrink_node(std::uint32_t node_id) noexcept;

  /// @brief Distance between objects in a slab, including red zone and alignment padding.
  [[nodiscard]] std::uint32_t size() const noexcept { return m_size; }
  /// @brief Usable size of an object.
  [[nodiscard]] std::uint32_t obj_size() const noexcept { return m_obj_size; }

  [[nodiscard, gnu::always_inline]] std::expected<void*, Error> alloc() noexcept {
    utils::IrqDisableGuard irq_guard;
    CpuCache& cpu = m_cpu_caches[hw::percpu::id()];

    if (void* obj = cpu.alloc(m_size)) [[likely]] {
      check_poison(obj);
      return obj;
    }

    auto obj = refill(cpu);
    if (obj) {
      check_poison(*obj);
    }

    return obj;
  }

  /// @brief Returns @p obj, which must have come from this cache, to it.
  void free(void* obj) noexcept;

  void apply_poison(void* obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::Poison)) {
      return;
    }

    // The first word is the free list link, so it cannot carry the pattern.
    if (m_obj_size > sizeof(void*)) {
      klib::memset(static_cast<char*>(obj) + sizeof(void*), POISON_FREE_BYTE,
                   m_obj_size - sizeof(void*));
    }
  }

  void check_poison(void* obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::Poison)) {
      return;
    }

    const std::size_t payload = m_obj_size > sizeof(void*) ? m_obj_size - sizeof(void*) : 0;
    const auto* mem = static_cast<const std::uint8_t*>(obj) + sizeof(void*);

    for (std::size_t i = 0; i < payload; ++i) {
      if (mem[i] != POISON_FREE_BYTE) [[unlikely]] {
        utils::logger::fatal("KmemCache: Use-After-Free detected at {} (offset {})\n", obj,
                             i + sizeof(void*));
      }
    }
  }

  // The guard word sits right after the object, so it is unaligned whenever obj_size is not a
  // multiple of 8.
  void setup_redzone(void* obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::RedZone)) {
      return;
    }

    constexpr std::uint64_t magic = REDZONE_MAGIC;
    __builtin_memcpy(static_cast<char*>(obj) + m_obj_size, &magic, sizeof(magic));
  }

  void check_redzone(void* obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::RedZone)) {
      return;
    }

    std::uint64_t magic;
    __builtin_memcpy(&magic, static_cast<const char*>(obj) + m_obj_size, sizeof(magic));
    if (magic != REDZONE_MAGIC) [[unlikely]] {
      utils::logger::fatal("KmemCache: Buffer overflow detected (Redzone corrupted) at {}\n", obj);
    }
  }
};

/// @brief Sets up the bootstrap caches and the kmalloc size classes. Needs a working PMM.
void initialize() noexcept;

/// @brief KmemCache::shrink_node() for every cache there is, with one round of IPIs for all.
/// @pre The calling thread can wait for other CPUs.
/// @return How many pages went back to the PMM.
std::size_t shrink_node(std::uint32_t node_id) noexcept;

/// @brief Allocates @p size bytes aligned to @p alignment (a power of two).
/// @return nullptr if @p size is zero, the alignment is invalid, or memory ran out.
[[nodiscard]] void* kmalloc(std::size_t size, std::size_t alignment = sizeof(void*)) noexcept;

/// @brief Frees memory from kmalloc()/krealloc(), or any KmemCache. Null is ignored.
void kfree(void* obj) noexcept;

/// @brief Resizes an allocation, moving it if it does not fit in place.
///
/// Follows realloc(): a null @p ptr behaves like kmalloc(), a zero @p new_size like kfree().
/// If the allocation has to move and that fails, the old block is left untouched and nullptr
/// is returned.
[[nodiscard]] void* krealloc(void* ptr, std::size_t new_size,
                             std::size_t align = sizeof(void*)) noexcept;
}  // namespace kernel::memory::heap
