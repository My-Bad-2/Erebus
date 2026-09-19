#pragma once

#include <cstdint>
#include <expected>
#include <new>
#include <span>

#include "core/errors.hpp"
#include "crypto/blake2b_prng.hpp"
#include "pmm/page.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::heap {
enum class CacheFlags : std::uint8_t {
  None = 0,
  RedZone = 1 << 0, // Adds bounds-checking magic bytes
  Poison = 1 << 1,  // Fills freed objects with poison to cache Use-After-Free
};

constexpr CacheFlags operator|(CacheFlags a, CacheFlags b) {
  return static_cast<CacheFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool has_flag(CacheFlags flags, CacheFlags flag) {
  return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

struct alignas(std::hardware_destructive_interference_size) CpuCache {
  void *local_freelist{nullptr};
  char *bump_ptr{nullptr};
  std::uint16_t bump_left{0};

  std::uint8_t partial_count{0};
  pmm::Page *page{nullptr};
  pmm::Page *cpu_partial{nullptr};

  static constexpr std::uint8_t MAX_CPU_PARTIAL = 4;
  [[nodiscard]] void *alloc(std::uint32_t object_size, const KmemCache *cache) noexcept;
};

struct alignas(std::hardware_destructive_interference_size) NodeCache {
  utils::QSpinlock lock;
  std::uint32_t num_partial{0};
  pmm::Page *partial_head{nullptr};
  pmm::Page *partial_tail{nullptr};

  static constexpr std::uint32_t MIN_PARTIAL_PAGES = 8;
};

class alignas(std::hardware_destructive_interference_size) KmemCache {
  std::span<CpuCache> m_cpu_caches;
  std::span<NodeCache> m_node_caches;

  std::uintptr_t m_random_cookie;
  void (*m_ctor)(void *);

  std::uint32_t m_obj_size;
  std::uint32_t m_size;
  std::uint8_t m_order;
  CacheFlags m_flags;
  std::uint16_t m_reserved;

  static constexpr std::uint8_t POISON_FREE_BYTE = 0x6B;
  static constexpr std::uint64_t REDZONE_MAGIC = 0xDEADBEEFCAFEBABE;

  [[nodiscard, gnu::cold, gnu::noinline]] std::expected<void *, Error> refill(CpuCache &cpu,
                                                                              std::uint32_t cpu_id) noexcept;
  void abandon_active_page(CpuCache &cpu, std::uint32_t cpu_id) noexcept;
  void push_to_node_partial(pmm::Page *page, std::uint32_t node_id, bool mostly_empty) noexcept;
  void remove_from_node_partial(pmm::Page *page, std::uint32_t node_id) noexcept;
  [[nodiscard]] pmm::Page *pop_from_node_partial(std::uint32_t node_id) noexcept;

public:
  constexpr KmemCache(const std::uint32_t obj_size, const std::uint32_t size, const std::uint32_t order,
                      const CacheFlags flags, void (*ctor)(void *), const std::span<CpuCache> cpus,
                      const std::span<NodeCache> nodes) noexcept
      : m_cpu_caches(cpus), m_node_caches(nodes), m_ctor(ctor), m_obj_size(obj_size), m_size(size), m_order(order),
        m_flags(flags), m_reserved(0) {
    hw::percpu::rng().get_random(m_random_cookie);
  }

  KmemCache(const KmemCache &) = delete;
  KmemCache &operator=(const KmemCache &) = delete;

  void destroy() noexcept;

  [[nodiscard]] std::uint32_t size() const noexcept { return m_size; }
  [[nodiscard]] std::uint32_t obj_size() const noexcept { return m_obj_size > 0 ? m_obj_size : m_size; }

  [[nodiscard]] static std::expected<KmemCache *, Error> create(std::uint32_t obj_size, std::uint32_t alignment,
                                                                CacheFlags flags = CacheFlags::None,
                                                                void (*ctor)(void *) = nullptr) noexcept;

  [[nodiscard, gnu::always_inline]] std::expected<void *, Error> alloc() noexcept {
    utils::IrqDisableGuard irq_guard;
    const std::uint32_t cpu_id = hw::percpu::id();
    CpuCache &cpu = m_cpu_caches[cpu_id];

    if (void *obj = cpu.alloc(m_size, this)) [[likely]] {
      check_poison(obj);
      return obj;
    }

    return refill(cpu, cpu_id);
  }

  void free(void *obj) noexcept;

  [[nodiscard]] void *obfuscate_ptr(void *ptr, void *obj_addr) const noexcept {
    return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(ptr) ^ m_random_cookie ^
                                    reinterpret_cast<std::uintptr_t>(obj_addr));
  }

  [[nodiscard]] void *reveal_ptr(void *ptr, void *obj_addr) const noexcept { return obfuscate_ptr(ptr, obj_addr); }

  void apply_poison(void *obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::Poison)) {
      return;
    }

    const std::size_t payload = m_obj_size > sizeof(void *) ? m_obj_size - sizeof(void *) : 0;
    if (payload > 0) {
      klib::memset(static_cast<char *>(obj) + sizeof(void *), POISON_FREE_BYTE, payload);
    }
  }

  void check_poison(void *obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::Poison) || m_ctor) {
      return;
    }

    const std::size_t payload = m_obj_size > sizeof(void *) ? m_obj_size - sizeof(void *) : 0;
    const auto *mem = static_cast<std::uint8_t *>(obj) + sizeof(void *);

    for (std::size_t i = 0; i < payload; ++i) {
      if (mem[i] != POISON_FREE_BYTE) [[unlikely]] {
        utils::logger::fatal("KmemCache: Use-After-Free detected at {} (offset {})\n", obj, i);
      }
    }
  }

  void setup_redzone(void *obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::RedZone)) {
      return;
    }

    *reinterpret_cast<std::uint64_t *>(static_cast<char *>(obj) + m_obj_size) = REDZONE_MAGIC;
  }

  void check_redzone(void *obj) const noexcept {
    if (!has_flag(m_flags, CacheFlags::RedZone)) {
      return;
    }

    const auto magic = *reinterpret_cast<std::uint64_t *>(static_cast<char *>(obj) + m_obj_size);
    if (magic != REDZONE_MAGIC) [[unlikely]] {
      utils::logger::fatal("KmemCache: Buffer overflow detected (Redzone corrupted) at {}\n", obj);
    }
  }
};

void initialize() noexcept;

[[nodiscard]] void *kmalloc(std::size_t size, std::size_t alignment = sizeof(void *)) noexcept;
void kfree(void *obj) noexcept;
[[nodiscard]] void *krealloc(void *ptr, std::size_t new_size, std::size_t align = sizeof(void *)) noexcept;
} // namespace kernel::memory::heap