#include "memory/heap.hpp"

#include "boot/boot.hpp"
#include "memory/address.hpp"
#include "memory/pmm.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "string.h"
#include "utils/logger.hpp"
#include <new>

namespace kernel::memory::heap {
namespace {
constexpr std::uint16_t SLUB_NO_IDX = 0xFFFF;

constexpr std::uint32_t KMALLOC_MIN_SHIFT = 3;  // 2^3 = 8 bytes
constexpr std::uint32_t KMALLOC_MAX_SHIFT = 15; // 2^15 = 32KB
constexpr std::uint32_t NUM_KMALLOC_CACHES = KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1;

KmemCache *g_meta_cache = nullptr;
KmemCache *g_meta_cpu_array = nullptr;
KmemCache *g_meta_node_array = nullptr;

KmemCache *g_kmalloc_caches[NUM_KMALLOC_CACHES] = {nullptr};

char *page_to_virt(const pmm::Page *page) noexcept {
  return DirectMap::phys_to_virt(pmm::page_to_phys(page)).as<char>();
}

pmm::Page *virt_to_page(void *ptr) noexcept {
  const auto phys = DirectMap::virt_to_phys(VirtualAddress{ptr});
  if (!phys) [[unlikely]] {
    return nullptr;
  }

  return pmm::phys_to_page(*phys);
}

[[nodiscard]] std::uint32_t size_to_index(const std::size_t size) noexcept {
  const std::size_t aligned_size = std::bit_ceil(std::max<std::size_t>(size, 1 << KMALLOC_MIN_SHIFT));
  const std::uint32_t shift = std::countr_zero(aligned_size);
  return shift - KMALLOC_MIN_SHIFT;
}
} // namespace

void initialize() noexcept {
  const std::uint32_t total_cpus = boot::mp_request.response->cpu_count;
  const std::uint32_t total_numa_nodes = pmm::g_topology.active_nodes();

  constexpr std::size_t base_sz = sizeof(KmemCache);
  const std::size_t cpu_sz = utils::maths::align_up(sizeof(CpuCache) * total_cpus, +64ul);
  const std::size_t node_sz = utils::maths::align_up(sizeof(NodeCache) * total_numa_nodes, 64ul);

  const std::size_t single_meta_sz = base_sz + cpu_sz + node_sz;
  const std::size_t total_boot_size = single_meta_sz * 3;

  std::uint32_t order = 0;
  while ((PAGE_SIZE << order) < total_boot_size) {
    order++;
  }

  const auto boot_phys = pmm::alloc_pages(pmm::PageMobility::Movable, order);
  if (!boot_phys) {
    utils::logger::fatal("Failed to allocate boot page for kmem cache!\n");
  }

  pmm::Page *boot_page = pmm::phys_to_page(*boot_phys);
  boot_page->set_state(pmm::PageState::Active);
  boot_page->set_mobility(pmm::PageMobility::Unmovable);
  char *base_ptr = page_to_virt(boot_page);

  auto carve_mother_cache = [&](const char *name, std::size_t obj_sz) -> KmemCache * {
    auto c = reinterpret_cast<KmemCache *>(base_ptr);
    base_ptr += base_sz;
    auto *ca = reinterpret_cast<CpuCache *>(base_ptr);
    base_ptr += cpu_sz;
    auto *na = reinterpret_cast<NodeCache *>(base_ptr);
    base_ptr += node_sz;

    std::span cpu_span(ca, total_cpus);
    for (auto &cpu : cpu_span) {
      new (&cpu) CpuCache();
    }

    std::span node_span(na, total_numa_nodes);
    for (auto &node : node_span) {
      new (&node) NodeCache();
    }

    std::uint32_t c_order = 0;
    while ((PAGE_SIZE << c_order) < obj_sz) {
      c_order++;
    }

    while (c_order < 3 && ((PAGE_SIZE << c_order) / obj_sz) < 32) {
      c_order++;
    }

    return new (c) KmemCache(name, obj_sz, c_order, cpu_span, node_span);
  };

  g_meta_cache = carve_mother_cache("kmem_meta_cache", sizeof(KmemCache));
  g_meta_cpu_array = carve_mother_cache("kmem_meta_cpu", sizeof(CpuCache) * total_numa_nodes);
  g_meta_node_array = carve_mother_cache("kmem_meta_node_array", sizeof(NodeCache) * total_numa_nodes);

  for (std::uint32_t i = 0; i < NUM_KMALLOC_CACHES; ++i) {
    const std::uint32_t size = 1 << (i + KMALLOC_MIN_SHIFT);

    const auto cache = KmemCache::create("generic-kmem-cache", size, sizeof(void *));
    if (cache) {
      g_kmalloc_caches[i] = *cache;
    } else {
      utils::logger::fatal("Failed to allocate generic caches for Kmalloc!");
    }
  }
}

void KmemCache::destroy() noexcept {
  for (auto &cpu : m_cpu_caches) {
    if (cpu.page) {
      cpu.page->set_state(pmm::PageState::Free);
      cpu.page->set_mobility(pmm::PageMobility::Movable);
      pmm::free_pages(pmm::page_to_phys(cpu.page), m_order);

      cpu.page = nullptr;
    }

    pmm::Page *current_partial = cpu.cpu_partial;
    while (current_partial) {
      pmm::Page *next = current_partial->slub.next_partial;

      current_partial->set_state(pmm::PageState::Free);
      current_partial->set_mobility(pmm::PageMobility::Movable);
      pmm::free_pages(pmm::page_to_phys(current_partial), m_order);

      current_partial = next;
    }

    cpu.cpu_partial = nullptr;
    cpu.partial_count = 0;
  }

  for (auto &[lock, num_partial, partial_head, partial_tail] : m_node_caches) {
    utils::IrqSaveGuard guard(lock);

    pmm::Page *current = partial_head;
    while (current) {
      pmm::Page *next = current->slub.next_partial;

      current->set_state(pmm::PageState::Free);
      current->set_mobility(pmm::PageMobility::Movable);
      pmm::free_pages(pmm::page_to_phys(current), m_order);

      current = next;
    }

    partial_head = nullptr;
    partial_tail = nullptr;
    num_partial = 0;
  }

  void *cpu_array_ptr = m_cpu_caches.data();
  void *node_array_ptr = m_node_caches.data();

  g_meta_cpu_array->free(cpu_array_ptr);
  g_meta_node_array->free(node_array_ptr);

  g_meta_cache->free(this);
}

std::expected<KmemCache *, Error> KmemCache::create(const std::string_view name, const std::uint32_t obj_size,
                                                    const std::uint32_t alignment) noexcept {
  const std::uint32_t total_cpus = boot::mp_request.response->cpu_count;
  const std::uint32_t total_numa_nodes = pmm::g_topology.active_nodes();

  const std::uint32_t align = std::max<std::uint32_t>(alignment, sizeof(void *));
  const std::uint32_t size = utils::maths::align_up(obj_size, align);

  std::uint32_t order = 0;
  while ((PAGE_SIZE << order) < size) {
    order++;
  }

  while (order < 3 && ((PAGE_SIZE << order) / size) < 32) {
    order++;
  }

  const auto cache_mem = g_meta_cache->alloc();
  const auto cpu_mem = g_meta_cpu_array->alloc();
  const auto node_mem = g_meta_node_array->alloc();

  if (!cache_mem || !cpu_mem || !node_mem) {
    return std::unexpected(Error::OutOfMemory);
  }

  auto *cache_ptr = static_cast<KmemCache *>(*cache_mem);
  auto *cpu_arr = static_cast<CpuCache *>(*cpu_mem);
  auto *node_arr = static_cast<NodeCache *>(*node_mem);

  std::span<CpuCache> cpu_span(cpu_arr, total_cpus);
  for (auto &c : cpu_span) {
    new (&c) CpuCache();
  }

  std::span<NodeCache> node_span(node_arr, total_numa_nodes);
  for (auto &n : node_span) {
    new (&n) NodeCache();
  }

  return new (cache_ptr) KmemCache(name, size, order, cpu_span, node_span);
}

void *CpuCache::alloc(const std::uint32_t object_size) noexcept {
  void *obj = local_freelist;
  if (obj != nullptr) [[likely]] {
    local_freelist = *static_cast<void **>(obj);
    if (local_freelist) {
      __builtin_prefetch(local_freelist, 1, 0);
    }

    return obj;
  }

  if (bump_left > 0) [[likely]] {
    obj = bump_ptr;
    bump_ptr += object_size;
    bump_left--;
    __builtin_prefetch(bump_ptr, 1, 0);
    return obj;
  }

  return nullptr;
}

std::expected<void *, Error> KmemCache::alloc() noexcept {
  utils::IrqDisableGuard irq_guard;
  const std::uint32_t cpu_id = hw::percpu::id();
  CpuCache &cpu = m_cpu_caches[cpu_id];

  void *obj = cpu.alloc(m_size);
  if (obj != nullptr) [[likely]] {
    return obj;
  }

  return refill(cpu, cpu_id);
}

std::expected<void *, Error> KmemCache::refill(CpuCache &cpu, const std::uint32_t cpu_id) noexcept {
  const std::uint32_t node_id = hw::percpu::numa_node();

  if (cpu.page) {
    SlabState state = cpu.page->read_slub_state();
    SlabState new_state;

    do {
      new_state = state;
      if (state.remote_head_idx == SLUB_NO_IDX) {
        break;
      }

      new_state.remote_head_idx = SLUB_NO_IDX;
      new_state.generation++;
    } while (!cpu.page->try_update_slub_state(state, new_state));

    if (state.remote_head_idx != SLUB_NO_IDX) {
      void *obj = page_to_virt(cpu.page) + (state.remote_head_idx * m_size);
      cpu.local_freelist = *static_cast<void **>(obj);
      return obj;
    }

    abandon_active_page(cpu, cpu_id);
  }

  if (cpu.cpu_partial) {
    pmm::Page *partial = cpu.cpu_partial;
    cpu.cpu_partial = partial->slub.next_partial;
    cpu.partial_count--;

    SlabState state = cpu.page->read_slub_state();
    SlabState new_state;

    do {
      new_state = state;
      new_state.frozen = 1;
      new_state.remote_head_idx = SLUB_NO_IDX;
    } while (!partial->try_update_slub_state(state, new_state));

    cpu.page = partial;
    cpu.bump_ptr = nullptr;
    cpu.bump_left = 0;

    void *obj = page_to_virt(partial) + (state.remote_head_idx * m_size);
    cpu.local_freelist = *static_cast<void **>(obj);
    return obj;
  }

  if (pmm::Page *partial = pop_from_node_partial(node_id)) {
    SlabState state = partial->read_slub_state();
    SlabState new_state;

    do {
      new_state = state;
      new_state.frozen = 1;
      new_state.remote_head_idx = SLUB_NO_IDX;
    } while (!partial->try_update_slub_state(state, new_state));

    cpu.page = partial;
    cpu.bump_ptr = nullptr;
    cpu.bump_left = 0;

    void *obj = page_to_virt(partial) + (state.remote_head_idx * m_size);
    cpu.local_freelist = *static_cast<void **>(obj);
    return obj;
  }

  const auto raw_page = pmm::alloc_pages(pmm::PageMobility::Movable, m_order);
  if (!raw_page) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  pmm::Page *new_page = pmm::phys_to_page(*raw_page);
  new_page->set_mobility(pmm::PageMobility::Unmovable);

  const std::uint16_t total = static_cast<std::uint16_t>((PAGE_SIZE << m_order) / m_size);
  const SlabState init_state = {
      .remote_head_idx = SLUB_NO_IDX,
      .in_use = total,
      .frozen = 1,
      .total = total,
      .generation = 0,
  };
  new_page->slub.state.store(std::bit_cast<std::uint64_t>(init_state), std::memory_order_relaxed);
  new_page->slub.cache = this;
  new_page->set_state(pmm::PageState::Active);

  cpu.page = new_page;
  cpu.bump_ptr = page_to_virt(new_page) + m_size;
  cpu.bump_left = total - 1;
  cpu.local_freelist = nullptr;

  return page_to_virt(new_page);
}

void KmemCache::free(void *obj) noexcept {
  utils::IrqDisableGuard irq_guard;
  CpuCache &cpu = m_cpu_caches[hw::percpu::id()];
  pmm::Page *page = virt_to_page(obj);

  if (page == cpu.page) [[likely]] {
    *static_cast<void **>(obj) = cpu.local_freelist;
    cpu.local_freelist = obj;
    return;
  }

  __builtin_prefetch(&page->slub.state, 1, 0);

  char *base = page_to_virt(page);
  const std::uint32_t offset = static_cast<std::uint32_t>(static_cast<char *>(obj) - base);
  const std::uint16_t obj_idx = static_cast<std::uint16_t>(offset / m_size);

  SlabState state = page->read_slub_state();
  SlabState new_state;
  do {
    new_state = state;

    void *next_ptr = (state.remote_head_idx != SLUB_NO_IDX) ? (base + (state.remote_head_idx * m_size)) : nullptr;

    *static_cast<void **>(obj) = next_ptr;
    new_state.remote_head_idx = obj_idx;
    new_state.in_use--;
    new_state.generation++;
  } while (!page->try_update_slub_state(state, new_state));

  if (new_state.frozen) [[likely]] {
    return;
  }

  const std::uint32_t node_id = page->get_numa_node();
  if (new_state.in_use == 0) [[unlikely]] {
    if (m_node_caches[node_id].num_partial < NodeCache::MIN_PARTIAL_PAGES) {
      remove_from_node_partial(page, node_id);
      push_to_node_partial(page, node_id, true);
    } else {
      remove_from_node_partial(page, node_id);
      pmm::free_pages(pmm::page_to_phys(page), m_order);
    }
  } else if (state.in_use == state.total) [[unlikely]] {
    const bool mostly_empty = new_state.in_use <= (new_state.total / 2);
    push_to_node_partial(page, node_id, mostly_empty);
  }
}

void KmemCache::abandon_active_page(CpuCache &cpu, std::uint32_t cpu_id) noexcept {
  pmm::Page *page = cpu.page;
  char *base = page_to_virt(page);
  void *bump_head = nullptr;
  void *bump_tail = nullptr;

  if (cpu.bump_left > 0) {
    char *current = cpu.bump_ptr;
    bump_head = current;
    for (std::uint16_t i = 0; i < cpu.bump_left; ++i) {
      void *next = (i == cpu.bump_left - 1) ? nullptr : (current + m_size);
      *reinterpret_cast<void **>(current) = next;
      bump_tail = current;
      current += m_size;
    }
  }

  SlabState state = page->read_slub_state();
  SlabState new_state;
  do {
    new_state = state;
    if (bump_tail && state.remote_head_idx != SLUB_NO_IDX) {
      *static_cast<void **>(bump_tail) = base + (state.remote_head_idx * m_size);
    }

    if (cpu.bump_left > 0) {
      const std::uint32_t head_offset = static_cast<std::uint32_t>(static_cast<char *>(bump_head) - base);
      new_state.remote_head_idx = static_cast<std::uint16_t>(head_offset / m_size);
    }

    new_state.frozen = 0;
    new_state.generation++;
  } while (!page->try_update_slub_state(state, new_state));

  const std::uint32_t node_id = page->get_numa_node();
  if (new_state.in_use > 0 && cpu.partial_count < CpuCache::MAX_CPU_PARTIAL) {
    page->slub.next_partial = cpu.cpu_partial;
    cpu.cpu_partial = page;
    cpu.partial_count++;
  } else if (new_state.in_use == 0) {
    pmm::free_pages(pmm::page_to_phys(page), m_order);
  } else {
    const bool mostly_empty = new_state.in_use <= (new_state.total / 2);
    push_to_node_partial(page, node_id, mostly_empty);
  }

  cpu.page = nullptr;
  cpu.bump_ptr = nullptr;
  cpu.bump_left = 0;
}

void KmemCache::push_to_node_partial(pmm::Page *page, std::uint32_t node_id, bool mostly_empty) noexcept {
  auto &[lock, num_partial, partial_head, partial_tail] = m_node_caches[node_id];
  utils::NakedGuard guard(lock);

  if (mostly_empty) {
    // Push to head
    page->slub.next_partial = partial_head;
    page->slub.prev_partial = nullptr;

    if (partial_head) {
      partial_head->slub.prev_partial = page;
    } else {
      partial_tail = page;
    }

    partial_head = page;
  } else {
    // Push to tail
    page->slub.prev_partial = partial_tail;
    page->slub.next_partial = nullptr;

    if (partial_tail) {
      partial_tail->slub.next_partial = page;
    } else {
      partial_head = page;
    }

    partial_tail = page;
  }

  num_partial++;
}

void KmemCache::remove_from_node_partial(pmm::Page *page, std::uint32_t node_id) noexcept {
  auto &[lock, num_partial, partial_head, partial_tail] = m_node_caches[node_id];
  utils::NakedGuard guard(lock);

  if (page->slub.prev_partial) {
    page->slub.prev_partial->slub.next_partial = page->slub.next_partial;
  } else {
    partial_head = page->slub.next_partial;
  }

  if (page->slub.next_partial) {
    page->slub.next_partial->slub.prev_partial = page->slub.prev_partial;
  } else {
    partial_tail = page->slub.prev_partial;
  }

  page->slub.next_partial = nullptr;
  page->slub.prev_partial = nullptr;
  num_partial--;
}

pmm::Page *KmemCache::pop_from_node_partial(std::uint32_t node_id) noexcept {
  auto &[lock, num_partial, partial_head, partial_tail] = m_node_caches[node_id];
  utils::NakedGuard guard(lock);

  pmm::Page *page = partial_head;
  if (page) {
    partial_head = page->slub.next_partial;
    if (partial_head) {
      partial_head->slub.prev_partial = nullptr;
    } else {
      partial_tail = nullptr;
    }

    page->slub.next_partial = nullptr;
    page->slub.prev_partial = nullptr;
    num_partial--;
  }

  return page;
}

void *kmalloc(const std::size_t size, const std::size_t alignment) noexcept {
  if (size == 0) [[unlikely]] {
    return nullptr;
  }

  const std::size_t required_size = std::max(size, alignment);
  const std::uint32_t index = size_to_index(required_size);

  if (index < NUM_KMALLOC_CACHES) [[likely]] {
    KmemCache *cache = g_kmalloc_caches[index];
    const auto res = cache->alloc();
    return res ? *res : nullptr;
  }

  // The request is > 32 kb. Directly ask the PMM
  std::uint8_t order = 0;
  while ((PAGE_SIZE << order) < required_size) {
    order++;
  }

  auto raw_page = pmm::alloc_pages(pmm::PageMobility::Movable, order);
  if (!raw_page) {
    return nullptr;
  }

  pmm::Page *page = pmm::phys_to_page(*raw_page);
  page->set_mobility(pmm::PageMobility::Unmovable);
  page->set_state(pmm::PageState::Active);

  page->slub.cache = nullptr;
  return page_to_virt(page);
}

void kfree(void *ptr) noexcept {
  if (!ptr) [[unlikely]] {
    return;
  }

  const pmm::Page *page = virt_to_page(ptr);
  KmemCache *cache = page->slub.cache;

  if (cache) [[likely]] {
    cache->free(ptr);
  } else {
    const std::uint8_t order = page->get_order();
    pmm::free_pages(pmm::page_to_phys(page), order);
  }
}

void *krealloc(void *ptr, const std::size_t new_size, const std::size_t align) noexcept {
  if (new_size == 0) {
    kfree(ptr);
    return nullptr;
  }

  if (ptr == nullptr) {
    return kmalloc(new_size, align);
  }

  const pmm::Page *page = virt_to_page(ptr);
  const KmemCache *old_cache = page->slub.cache;

  std::size_t old_size;
  if (old_cache != nullptr) {
    old_size = old_cache->size();
  } else {
    old_size = PAGE_SIZE << page->get_order();
  }

  const std::size_t required_size = std::max(new_size, align);
  if (old_size >= required_size) {
    if (utils::maths::is_aligned(reinterpret_cast<std::uintptr_t>(ptr), align)) {
      return ptr;
    }
  }

  void *new_ptr = kmalloc(new_size, align);
  if (!new_ptr) {
    return nullptr;
  }

  klib::memcpy(new_ptr, ptr, std::min(old_size, new_size));
  kfree(ptr);
  return new_ptr;
}
} // namespace kernel::memory::heap

using namespace kernel::memory::heap;
[[nodiscard]] void *operator new(const std::size_t size) {
  void *ptr = kmalloc(size);
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in operator new");
  }

  return ptr;
}

[[nodiscard]] void *operator new[](const std::size_t size) {
  void *ptr = kmalloc(size);
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in operator new[]");
  }

  return ptr;
}

[[nodiscard]] void *operator new(const std::size_t size, const std::nothrow_t &) noexcept { return kmalloc(size); }
[[nodiscard]] void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept { return kmalloc(size); }

[[nodiscard]] void *operator new(std::size_t size, std::align_val_t alignment) {
  void *ptr = kmalloc(size, static_cast<std::size_t>(alignment));
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in aligne doperator new");
  }

  return ptr;
}

[[nodiscard]] void *operator new[](const std::size_t size, std::align_val_t alignment) {
  void *ptr = kmalloc(size, static_cast<std::size_t>(alignment));
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in aligned operator new[]");
  }

  return ptr;
}

[[nodiscard]] void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return kmalloc(size, static_cast<std::size_t>(alignment));
}

[[nodiscard]] void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return kmalloc(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *ptr) noexcept { kfree(ptr); }
void operator delete[](void *ptr) noexcept { kfree(ptr); }

void operator delete(void *ptr, std::size_t /* size */) noexcept { kfree(ptr); }
void operator delete[](void *ptr, std::size_t /* size */) noexcept { kfree(ptr); }

void operator delete(void *ptr, std::align_val_t /* alignment */) noexcept { kfree(ptr); }
void operator delete[](void *ptr, std::align_val_t /* alignment */) noexcept { kfree(ptr); }

void operator delete(void *ptr, std::size_t /* size */, std::align_val_t /* alignment */) noexcept { kfree(ptr); }
void operator delete[](void *ptr, std::size_t /* size */, std::align_val_t /* alignment */) noexcept { kfree(ptr); }