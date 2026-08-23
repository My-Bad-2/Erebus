#pragma once

#include <cstdint>
#include <expected>
#include <new>
#include <span>
#include <string_view>

#include "pmm/page.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::memory::heap {
enum class Error {
  OutOfMemory,
  InvalidAlignment,
};

struct alignas(std::hardware_destructive_interference_size) CpuCache {
  void *local_freelist{nullptr};
  char *bump_ptr{nullptr};

  pmm::Page *page{nullptr};
  pmm::Page *cpu_partial{nullptr};

  std::uint16_t bump_left{0};
  std::uint8_t partial_count{0};

  static constexpr std::uint8_t MAX_CPU_PARTIAL = 4;
  [[nodiscard]] void *alloc(std::uint32_t object_size) noexcept;
};

struct NodeCache {
  utils::QSpinlock lock;
  std::uint32_t num_partial{0};
  pmm::Page *partial_head{nullptr};
  pmm::Page *partial_tail{nullptr};

  static constexpr std::uint32_t MIN_PARTIAL_PAGES = 8;
};

class alignas(std::hardware_destructive_interference_size) KmemCache {
  std::string_view m_name;
  std::uint32_t m_size;
  std::uint32_t m_order;
  std::span<CpuCache> m_cpu_caches;
  std::span<NodeCache> m_node_caches;

  [[nodiscard]] std::expected<void *, Error> refill(CpuCache &cpu, std::uint32_t cpu_id) noexcept;
  void abandon_active_page(CpuCache &cpu, std::uint32_t cpu_id) noexcept;
  void push_to_node_partial(pmm::Page *page, std::uint32_t node_id, bool mostly_empty) noexcept;
  void remove_from_node_partial(pmm::Page *page, std::uint32_t node_id) noexcept;
  [[nodiscard]] pmm::Page *pop_from_node_partial(std::uint32_t node_id) noexcept;

public:
  constexpr KmemCache(const std::string_view name, const std::uint32_t size, const std::uint32_t order,
                      const std::span<CpuCache> cpus, const std::span<NodeCache> nodes) noexcept
      : m_name(name), m_size(size), m_order(order), m_cpu_caches(cpus), m_node_caches(nodes) {}

  KmemCache(const KmemCache &) = delete;
  KmemCache &operator=(const KmemCache &) = delete;

  [[nodiscard]] static std::expected<KmemCache *, Error> create(std::string_view name, std::uint32_t obj_size,
                                                                std::uint32_t alignment) noexcept;

  [[nodiscard]] std::expected<void *, Error> alloc() noexcept;
  void free(void *obj) noexcept;
};

void initialize() noexcept;
} // namespace kernel::memory::heap