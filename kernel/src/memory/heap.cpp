#include "memory/heap.hpp"

#include <string.h>

#include <new>

#include "hal/smp/ipi.hpp"
#include "hal/smp/smp.hpp"
#include "memory/address.hpp"
#include "memory/pmm.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/shrinker.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::heap {
namespace {
constexpr std::uint16_t SLUB_NO_IDX = 0xFFFF;

constexpr std::uint32_t KMALLOC_MIN_SHIFT = 3;   // 2^3 = 8B
constexpr std::uint32_t KMALLOC_MAX_SHIFT = 15;  // 2^15 = 32KB
constexpr std::uint32_t NUM_KMALLOC_CACHES = KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1;
constexpr std::size_t MIN_ALLOC_SIZE = std::size_t{1} << KMALLOC_MIN_SHIFT;

constexpr std::uint32_t MIN_OBJECTS_PER_SLAB = 32;
constexpr std::uint8_t MAX_SLAB_ORDER = 3;

static_assert((PAGE_SIZE << MAX_SLAB_ORDER) / sizeof(void*) <= 0x7FFF);

KmemCache* g_meta_cache = nullptr;
KmemCache* g_meta_cpu_array = nullptr;
KmemCache* g_meta_node_array = nullptr;

KmemCache* g_kmalloc_caches[NUM_KMALLOC_CACHES] = {nullptr};

constinit utils::QSpinlock g_cache_list_lock{};
KmemCache* g_cache_list = nullptr;

std::size_t shrink_heap(const std::uint32_t node_id, void*) noexcept {
  return shrink_node(node_id);
}

constinit Shrinker g_heap_shrinker{&shrink_heap, nullptr};

[[nodiscard]] bool is_on_node(const std::uint32_t cpu, const void* node_id) noexcept {
  return hw::smp::entry(cpu).numa_node == *static_cast<const std::uint32_t*>(node_id);
}

// Runs fn(arg) on every CPU of the node, this one included if it is one of them, the way an IPI
// would: with interrupts off.
void run_on_node_cpus(std::uint32_t node_id, const hw::ipi::Function fn, void* arg) noexcept {
  hw::percpu::preempt_disable();
  (void)hw::ipi::call_many(&is_on_node, &node_id, fn, arg, hw::ipi::Wait::Yes);

  if (hw::percpu::numa_node_id() == node_id) {
    utils::IrqDisableGuard irq_guard;
    fn(arg);
  }

  hw::percpu::preempt_enable();
}

char* page_to_virt(const pmm::Page* page) noexcept {
  return DirectMap::phys_to_virt(pmm::page_to_phys(page)).as<char>();
}

pmm::Page* virt_to_page(const void* ptr) noexcept {
  if (const auto phys = DirectMap::virt_to_phys(VirtualAddress{ptr})) [[likely]] {
    return pmm::phys_to_page(*phys);
  }

  return nullptr;
}

[[nodiscard]] constexpr std::uint32_t size_to_index(const std::size_t size) noexcept {
  if (size <= MIN_ALLOC_SIZE) {
    return 0;
  }

  return std::countr_zero(std::bit_ceil(size)) - KMALLOC_MIN_SHIFT;
}

[[nodiscard]] std::uint8_t slab_order_for(const std::uint32_t size) noexcept {
  std::uint8_t order = 0;
  while ((PAGE_SIZE << order) < size) {
    ++order;
  }

  while (order < MAX_SLAB_ORDER && (PAGE_SIZE << order) / size < MIN_OBJECTS_PER_SLAB) {
    ++order;
  }

  return order;
}

[[nodiscard]] bool mostly_empty(const SlabState& state) noexcept {
  return state.in_use <= state.total / 2;
}
}  // namespace

void initialize() noexcept {
  const std::uint32_t total_cpus = hw::smp::possible_cpus();
  const std::uint32_t total_numa_nodes = pmm::g_topology.active_nodes();

  constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
  const std::size_t cpu_bytes = utils::maths::align_up(sizeof(CpuCache) * total_cpus, CACHE_LINE);
  const std::size_t node_bytes =
      utils::maths::align_up(sizeof(NodeCache) * total_numa_nodes, CACHE_LINE);
  const std::size_t per_cache_bytes = sizeof(KmemCache) + cpu_bytes + node_bytes;

  // The three caches that allocate cache descriptors cannot allocate their own, so they are
  // carved out of one block taken straight from the PMM. That block is never freed.
  std::uint8_t order = 0;
  while ((PAGE_SIZE << order) < per_cache_bytes * 3) {
    order++;
  }

  const auto boot_phys = pmm::alloc_pages(pmm::PageMobility::Unmovable, order);
  if (!boot_phys) {
    utils::logger::fatal("Failed to allocate boot page for kmem cache!\n");
  }

  char* cursor = DirectMap::phys_to_virt(*boot_phys).as<char>();

  const auto carve_bootstrap_cache = [&](const std::uint32_t obj_size) -> KmemCache* {
    char* cache_mem = cursor;
    cursor += sizeof(KmemCache);
    auto* cpus = reinterpret_cast<CpuCache*>(cursor);
    cursor += cpu_bytes;
    auto* nodes = reinterpret_cast<NodeCache*>(cursor);
    cursor += node_bytes;

    const std::span cpu_span(cpus, total_cpus);
    std::uninitialized_default_construct(cpu_span.begin(), cpu_span.end());

    const std::span node_span(nodes, total_numa_nodes);
    std::uninitialized_default_construct(node_span.begin(), node_span.end());

    // All three object sizes are multiples of the cache line, so no extra alignment is needed.
    return new (cache_mem) KmemCache(obj_size, obj_size, slab_order_for(obj_size), CacheFlags::None,
                                     nullptr, cpu_span, node_span);
  };

  g_meta_cache = carve_bootstrap_cache(sizeof(KmemCache));
  g_meta_cpu_array = carve_bootstrap_cache(sizeof(CpuCache) * total_cpus);
  g_meta_node_array = carve_bootstrap_cache(sizeof(NodeCache) * total_numa_nodes);

  for (KmemCache* cache : {g_meta_cache, g_meta_cpu_array, g_meta_node_array}) {
    KmemCache::add_to_cache_list(cache);
  }

  for (std::uint32_t i = 0; i < NUM_KMALLOC_CACHES; ++i) {
    const std::uint32_t size = 1u << (i + KMALLOC_MIN_SHIFT);

    if (auto cache = KmemCache::create(size, sizeof(void*))) {
      g_kmalloc_caches[i] = *cache;
    } else {
      utils::logger::fatal("Failed to allocate generic caches for Kmalloc!\n");
    }
  }

  register_shrinker(g_heap_shrinker);
}

void KmemCache::add_to_cache_list(KmemCache* cache) noexcept {
  utils::PreemptGuard guard{g_cache_list_lock};
  cache->m_next_cache = g_cache_list;
  g_cache_list = cache;
}

std::expected<KmemCache*, Error> KmemCache::create(const std::uint32_t obj_size,
                                                   const std::uint32_t alignment, CacheFlags flags,
                                                   void (*ctor)(void*)) noexcept {
  if (obj_size == 0 || !std::has_single_bit(alignment)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  // Poisoning would wipe out the constructed state the next alloc() expects to find.
  if (ctor) {
    flags = flags & ~CacheFlags::Poison;
  }

  std::uint32_t stride = obj_size;
  if (has_flag(flags, CacheFlags::RedZone)) {
    stride += sizeof(std::uint64_t);
  }

  // Objects sit at multiples of `size` from a block aligned to the slab size, so a stride that
  // is a multiple of the alignment is enough to align every object.
  const std::uint32_t align = std::max<std::uint32_t>(alignment, sizeof(void*));
  const std::uint32_t size = utils::maths::align_up(stride, align);
  const std::uint8_t order = slab_order_for(size);
  if (order > pmm::MAX_ORDER) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const auto cache_mem = g_meta_cache->alloc();
  if (!cache_mem) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  const auto cpu_mem = g_meta_cpu_array->alloc();
  if (!cpu_mem) [[unlikely]] {
    g_meta_cache->free(*cache_mem);
    return std::unexpected(Error::OutOfMemory);
  }

  const auto node_mem = g_meta_node_array->alloc();
  if (!node_mem) [[unlikely]] {
    g_meta_cpu_array->free(*cpu_mem);
    g_meta_cache->free(*cache_mem);
    return std::unexpected(Error::OutOfMemory);
  }

  const std::span cpu_span(static_cast<CpuCache*>(*cpu_mem), hw::smp::possible_cpus());
  std::uninitialized_default_construct(cpu_span.begin(), cpu_span.end());

  const std::span node_span(static_cast<NodeCache*>(*node_mem), pmm::g_topology.active_nodes());
  std::uninitialized_default_construct(node_span.begin(), node_span.end());

  auto* cache = new (*cache_mem) KmemCache(obj_size, size, order, flags, ctor, cpu_span, node_span);
  add_to_cache_list(cache);
  return cache;
}

void KmemCache::destroy() noexcept {
  // Off the list first, so that no shrink starts on what is about to go away. One that has it
  // already holds the list, so taking the lock is waiting for that to end.
  {
    utils::PreemptGuard guard{g_cache_list_lock};
    for (KmemCache** link = &g_cache_list; *link != nullptr; link = &(*link)->m_next_cache) {
      if (*link == this) {
        *link = m_next_cache;
        break;
      }
    }
  }

  utils::IrqDisableGuard irq_guard;

  // Slabs that still hold live objects are leaked rather than freed: handing them back to the
  // PMM would turn the caller's bug into silent memory corruption.
  std::uint64_t live_objects = 0;
  const auto release_if_unused = [&](pmm::Page* slab, const std::uint32_t live) {
    if (live == 0) {
      release_slab(slab);
    } else {
      live_objects += live;
    }
  };

  for (CpuCache& cpu : m_cpu_caches) {
    if (pmm::Page* slab = cpu.page) {
      std::uint32_t owned = cpu.bump_left;
      for (void* obj = cpu.local_freelist; obj; obj = *static_cast<void**>(obj)) {
        ++owned;
      }

      release_if_unused(slab, slab->read_slub_state().in_use - owned);
    }

    for (pmm::Page* slab = cpu.cpu_partial; slab;) {
      pmm::Page* next = slab->slub.next_partial;
      release_if_unused(slab, slab->read_slub_state().in_use);
      slab = next;
    }

    cpu = CpuCache{};
  }

  for (NodeCache& node : m_node_caches) {
    // Detach the whole list first; slabs go back to the PMM without the node lock held.
    pmm::Page* slab = nullptr;
    {
      utils::NakedGuard guard{node.lock};
      slab = node.partial_head;
      node.partial_head = node.partial_tail = nullptr;
      node.num_partial = 0;
    }

    while (slab) {
      pmm::Page* next = slab->slub.next_partial;
      slab->slub.on_node_list = false;
      release_if_unused(slab, slab->read_slub_state().in_use);
      slab = next;
    }
  }

  if (live_objects > 0) [[unlikely]] {
    // The leaked slabs still point at this cache, so it has to stay alive too.
    utils::logger::warn("KmemCache: destroyed with {} live objects, leaking the cache\n",
                        live_objects);
    return;
  }

  g_meta_cpu_array->free(m_cpu_caches.data());
  g_meta_node_array->free(m_node_caches.data());
  g_meta_cache->free(this);
}

void* CpuCache::alloc(const std::uint32_t object_size) noexcept {
  if (void* obj = local_freelist) [[likely]] {
    local_freelist = *static_cast<void**>(obj);
    if (local_freelist) {
      __builtin_prefetch(local_freelist, 1, 0);
    }

    return obj;
  }

  if (bump_left > 0) [[likely]] {
    void* obj = bump_ptr;
    bump_ptr += object_size;
    bump_left--;
    __builtin_prefetch(bump_ptr, 1, 0);
    return obj;
  }

  return nullptr;
}

pmm::Page* KmemCache::slab_of(const void* obj) const noexcept {
  const auto phys = DirectMap::virt_to_phys(VirtualAddress{obj});
  if (!phys) [[unlikely]] {
    utils::logger::fatal("KmemCache: {} is not a heap pointer\n", obj);
  }

  // Slabs are buddy blocks and therefore aligned to their own size.
  const std::uint64_t pfn = phys->value() >> PAGE_SHIFT_4KB;
  return pmm::pfn_to_page(pfn & ~((1ul << m_order) - 1));
}

std::uint16_t KmemCache::index_of(const pmm::Page* slab, const void* obj) const noexcept {
  return static_cast<std::uint16_t>((static_cast<const char*>(obj) - page_to_virt(slab)) / m_size);
}

void* KmemCache::object_at(const pmm::Page* slab, const std::uint16_t idx) const noexcept {
  return page_to_virt(slab) + (static_cast<std::size_t>(idx) * m_size);
}

void* KmemCache::claim_remote_objects(CpuCache& cpu, pmm::Page* slab) noexcept {
  SlabState state = slab->read_slub_state();
  SlabState new_state;

  do {
    if (state.remote_head_idx == SLUB_NO_IDX) {
      return nullptr;
    }

    // Everything that was on the remote list now belongs to this CPU, so by the accounting
    // invariant every object of the slab counts as in use again.
    new_state = state;
    new_state.remote_head_idx = SLUB_NO_IDX;
    new_state.in_use = state.total;
    new_state.frozen = 1;
    new_state.generation++;
  } while (!slab->try_update_slub_state(state, new_state));

  void* obj = object_at(slab, state.remote_head_idx);
  cpu.local_freelist = *static_cast<void**>(obj);
  return obj;
}

std::expected<pmm::Page*, Error> KmemCache::new_slab() noexcept {
  const auto phys = pmm::alloc_pages(pmm::PageMobility::Unmovable, m_order);
  if (!phys) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  pmm::Page* slab = pmm::phys_to_page(*phys);
  const auto total = static_cast<std::uint16_t>((PAGE_SIZE << m_order) / m_size);

  char* obj = page_to_virt(slab);
  for (std::uint16_t i = 0; i < total; ++i, obj += m_size) {
    if (m_ctor) {
      m_ctor(obj);
    } else {
      apply_poison(obj);
    }

    setup_redzone(obj);
  }

  // kfree() can be handed a pointer into any frame of the slab and finds the cache from there.
  for (std::size_t i = 0; i < (1ul << m_order); ++i) {
    slab[i].slub.cache = this;
  }

  slab->slub.prev_partial = nullptr;
  slab->slub.next_partial = nullptr;
  slab->slub.on_node_list = false;

  const SlabState init_state = {
      .remote_head_idx = SLUB_NO_IDX,
      .in_use = total,
      .frozen = 1,
      .total = total,
      .generation = 0,
  };
  slab->slub.state.store(std::bit_cast<std::uint64_t>(init_state), std::memory_order_relaxed);

  return slab;
}

std::expected<void*, Error> KmemCache::refill(CpuCache& cpu) noexcept {
  // Other CPUs may have freed objects into the active slab since we last looked.
  if (cpu.page) {
    if (void* obj = claim_remote_objects(cpu, cpu.page)) {
      return obj;
    }

    deactivate_slab(cpu);
  }

  pmm::Page* slab = cpu.cpu_partial;
  if (slab) [[likely]] {
    cpu.cpu_partial = slab->slub.next_partial;
    cpu.partial_count--;
  } else {
    slab = pop_from_node_partial(hw::percpu::numa_node_id());
  }

  if (slab) {
    // Partial slabs always have objects on their remote list (in_use < total), and nobody but
    // the owner can take them off. Now that we own it, an empty list means broken bookkeeping.
    void* obj = claim_remote_objects(cpu, slab);
    if (!obj) [[unlikely]] {
      utils::logger::fatal("KmemCache: Pulled a completely full page from partial list!\n");
    }

    cpu.page = slab;
    cpu.bump_ptr = nullptr;
    cpu.bump_left = 0;
    return obj;
  }

  const auto fresh = new_slab();
  if (!fresh) [[unlikely]] {
    return std::unexpected(fresh.error());
  }

  const SlabState state = (*fresh)->read_slub_state();
  char* first = page_to_virt(*fresh);

  cpu.page = *fresh;
  cpu.local_freelist = nullptr;
  cpu.bump_ptr = first + m_size;
  cpu.bump_left = state.total - 1;
  return first;
}

void KmemCache::free(void* obj) noexcept {
  check_redzone(obj);
  apply_poison(obj);

  pmm::Page* slab = slab_of(obj);

  utils::IrqDisableGuard irq_guard;
  CpuCache& cpu = m_cpu_caches[hw::percpu::id()];

  // Our own active slab: no atomics needed, the object just goes on the local free list.
  if (slab == cpu.page) [[likely]] {
    *static_cast<void**>(obj) = cpu.local_freelist;
    cpu.local_freelist = obj;
    return;
  }

  const std::uint16_t obj_idx = index_of(slab, obj);

  SlabState state = slab->read_slub_state();
  SlabState new_state;
  do {
    *static_cast<void**>(obj) =
        (state.remote_head_idx != SLUB_NO_IDX) ? object_at(slab, state.remote_head_idx) : nullptr;

    new_state = state;
    new_state.remote_head_idx = obj_idx;
    new_state.in_use--;
    new_state.generation++;
  } while (!slab->try_update_slub_state(state, new_state));

  // A frozen slab belongs to a CPU, which sorts it out when it lets go of it.
  if (new_state.frozen) [[likely]] {
    return;
  }

  if (state.in_use == state.total) {
    // Full slabs are on no list, and only the free that takes one below full sees this
    // transition, so we are the only one who can publish it.
    publish_partial(slab);
  } else if (new_state.in_use == 0) {
    discard_if_empty(slab);
  }
}

void KmemCache::deactivate_slab(CpuCache& cpu) noexcept {
  pmm::Page* slab = cpu.page;
  if (!slab) {
    return;
  }

  // Chain the local free list and the unused bump region into one list that is spliced onto
  // the slab's remote list below.
  void* chain_head = nullptr;
  void* chain_tail = nullptr;
  std::uint16_t owned = 0;

  for (void* obj = cpu.local_freelist; obj; obj = *static_cast<void**>(obj)) {
    chain_head = chain_head ? chain_head : obj;
    chain_tail = obj;
    ++owned;
  }

  char* bump = cpu.bump_ptr;
  for (std::uint16_t i = 0; i < cpu.bump_left; ++i, bump += m_size) {
    if (chain_tail) {
      *static_cast<void**>(chain_tail) = bump;
    } else {
      chain_head = bump;
    }

    chain_tail = bump;
    ++owned;
  }

  cpu.page = nullptr;
  cpu.local_freelist = nullptr;
  cpu.bump_ptr = nullptr;
  cpu.bump_left = 0;

  const std::uint16_t head_idx = chain_head ? index_of(slab, chain_head) : SLUB_NO_IDX;

  SlabState state = slab->read_slub_state();
  SlabState new_state;
  bool to_cpu_partial = false;

  do {
    new_state = state;

    if (chain_tail) {
      *static_cast<void**>(chain_tail) =
          (state.remote_head_idx != SLUB_NO_IDX) ? object_at(slab, state.remote_head_idx) : nullptr;
      new_state.remote_head_idx = head_idx;
    }

    new_state.in_use = state.in_use - owned;

    // A partially used slab is worth keeping close; it stays frozen on this CPU's list.
    to_cpu_partial = new_state.in_use > 0 && new_state.in_use < new_state.total &&
                     cpu.partial_count < CpuCache::MAX_CPU_PARTIAL;
    new_state.frozen = to_cpu_partial ? 1 : 0;
    new_state.generation++;
  } while (!slab->try_update_slub_state(state, new_state));

  if (to_cpu_partial) {
    slab->slub.next_partial = cpu.cpu_partial;
    cpu.cpu_partial = slab;
    cpu.partial_count++;
  } else if (new_state.in_use < new_state.total) {
    // Partial or empty. publish_partial() decides whether an empty slab is worth keeping.
    publish_partial(slab);
  }

  // Otherwise every object is live. The slab stays on no list until a free makes it partial.
}

void KmemCache::release_slab(pmm::Page* slab) const noexcept {
  pmm::free_pages(pmm::page_to_phys(slab), m_order);
}

void KmemCache::publish_partial(pmm::Page* slab) noexcept {
  NodeCache& node = m_node_caches[slab->get_numa_node()];
  bool release = false;

  {
    utils::NakedGuard guard(node.lock);

    // Frees keep landing while we wait for the lock, so look at the state again. Nobody else
    // can claim the slab: it is unfrozen and not on any list yet.
    const SlabState state = slab->read_slub_state();
    if (state.in_use == 0 && node.num_partial >= NodeCache::MIN_PARTIAL_PAGES) {
      release = true;
    } else if (mostly_empty(state)) {
      slab->slub.prev_partial = nullptr;
      slab->slub.next_partial = node.partial_head;

      if (node.partial_head) {
        node.partial_head->slub.prev_partial = slab;
      } else {
        node.partial_tail = slab;
      }

      node.partial_head = slab;
    } else {
      slab->slub.prev_partial = node.partial_tail;
      slab->slub.next_partial = nullptr;

      if (node.partial_tail) {
        node.partial_tail->slub.next_partial = slab;
      } else {
        node.partial_head = slab;
      }

      node.partial_tail = slab;
    }

    if (!release) {
      slab->slub.on_node_list = true;
      node.num_partial++;
    }
  }

  // Give the slab back after dropping the node lock: the PMM takes zone locks of its own, and
  // keeping the two lock classes unnested means there is no ordering between them to get wrong.
  if (release) {
    release_slab(slab);
  }
}

void KmemCache::discard_if_empty(pmm::Page* slab) noexcept {
  NodeCache& node = m_node_caches[slab->get_numa_node()];

  {
    utils::NakedGuard guard(node.lock);

    // Off the list means either a CPU popped it and owns it now, or whoever is about to publish
    // it has not got there yet. Either way the slab is not ours to touch.
    if (!slab->slub.on_node_list || slab->read_slub_state().in_use != 0) {
      return;
    }

    if (node.num_partial <= NodeCache::MIN_PARTIAL_PAGES) {
      return;
    }

    unlink_partial_locked(node, slab);
  }

  release_slab(slab);
}

void KmemCache::unlink_partial_locked(NodeCache& node, pmm::Page* slab) noexcept {
  if (slab->slub.prev_partial) {
    slab->slub.prev_partial->slub.next_partial = slab->slub.next_partial;
  } else {
    node.partial_head = slab->slub.next_partial;
  }

  if (slab->slub.next_partial) {
    slab->slub.next_partial->slub.prev_partial = slab->slub.prev_partial;
  } else {
    node.partial_tail = slab->slub.prev_partial;
  }

  slab->slub.next_partial = nullptr;
  slab->slub.prev_partial = nullptr;
  slab->slub.on_node_list = false;
  node.num_partial--;
}

void KmemCache::unfreeze(pmm::Page* slab) noexcept {
  SlabState state = slab->read_slub_state();
  SlabState new_state;

  do {
    new_state = state;
    new_state.frozen = 0;
    new_state.generation++;
  } while (!slab->try_update_slub_state(state, new_state));

  // From here on it is like any slab nobody owns, and frees go by the rules for those. Being on
  // no list, a partial one is ours to publish; a full one waits for the free that changes that.
  if (new_state.in_use < new_state.total) {
    publish_partial(slab);
  }
}

void KmemCache::flush_cpu_partials_local() noexcept {
  CpuCache& cpu = m_cpu_caches[hw::percpu::id()];
  pmm::Page* slab = std::exchange(cpu.cpu_partial, nullptr);
  cpu.partial_count = 0;

  while (slab != nullptr) {
    pmm::Page* next = slab->slub.next_partial;
    unfreeze(slab);
    slab = next;
  }
}

void KmemCache::flush_cpu_partials_of(void* cache) noexcept {
  static_cast<KmemCache*>(cache)->flush_cpu_partials_local();
}

void KmemCache::flush_every_cpu_partial(void*) noexcept {
  // Whoever sent this holds the list of caches until every CPU is done, so it cannot change.
  for (KmemCache* cache = g_cache_list; cache != nullptr; cache = cache->m_next_cache) {
    cache->flush_cpu_partials_local();
  }
}

std::size_t KmemCache::trim_node(const std::uint32_t node_id) noexcept {
  NodeCache& node = m_node_caches[node_id];
  pmm::Page* empty = nullptr;
  std::size_t count = 0;

  {
    utils::IrqSaveGuard guard{node.lock};

    // Nobody allocates from a slab on the list, and a slab with nothing in use gets no frees, so
    // what is empty here stays empty.
    for (pmm::Page* slab = node.partial_head; slab != nullptr;) {
      pmm::Page* next = slab->slub.next_partial;

      if (slab->read_slub_state().in_use == 0) {
        unlink_partial_locked(node, slab);
        slab->slub.next_partial = empty;
        empty = slab;
        ++count;
      }

      slab = next;
    }
  }

  while (empty != nullptr) {
    pmm::Page* next = empty->slub.next_partial;
    empty->slub.next_partial = nullptr;
    release_slab(empty);
    empty = next;
  }

  return count << m_order;
}

std::size_t KmemCache::shrink_node(const std::uint32_t node_id) noexcept {
  if (node_id >= m_node_caches.size()) [[unlikely]] {
    return 0;
  }

  run_on_node_cpus(node_id, &flush_cpu_partials_of, this);
  return trim_node(node_id);
}

std::size_t shrink_node(const std::uint32_t node_id) noexcept {
  if (node_id >= pmm::g_topology.active_nodes()) [[unlikely]] {
    return 0;
  }

  utils::PreemptGuard guard{g_cache_list_lock};

  run_on_node_cpus(node_id, &KmemCache::flush_every_cpu_partial, nullptr);

  std::size_t pages = 0;
  for (KmemCache* cache = g_cache_list; cache != nullptr; cache = cache->m_next_cache) {
    pages += cache->trim_node(node_id);
  }

  return pages;
}

pmm::Page* KmemCache::pop_from_node_partial(const std::uint32_t node_id) noexcept {
  NodeCache& node = m_node_caches[node_id];
  utils::NakedGuard guard(node.lock);

  pmm::Page* slab = node.partial_head;
  if (slab) {
    node.partial_head = slab->slub.next_partial;

    if (node.partial_head) [[likely]] {
      node.partial_head->slub.prev_partial = nullptr;
    } else {
      node.partial_tail = nullptr;
    }

    slab->slub.next_partial = nullptr;
    slab->slub.prev_partial = nullptr;
    slab->slub.on_node_list = false;
    node.num_partial--;
  }

  return slab;
}

void* kmalloc(const std::size_t size, const std::size_t alignment) noexcept {
  if (size == 0 || !std::has_single_bit(alignment)) [[unlikely]] {
    return nullptr;
  }

  // kmalloc caches are powers of two and their slabs are aligned to their size, so rounding the
  // size up to the alignment is enough to get an aligned object.
  const std::size_t required_size = std::max(size, alignment);
  const std::uint32_t index = size_to_index(required_size);

  if (index < NUM_KMALLOC_CACHES) [[likely]] {
    const auto res = g_kmalloc_caches[index]->alloc();
    return res ? *res : nullptr;
  }

  // Too big for the caches: take whole pages. Buddy blocks are aligned to their size as well.
  const std::size_t page_count = utils::maths::div_round_up(required_size, PAGE_SIZE);
  const auto order = static_cast<std::uint8_t>(std::countr_zero(std::bit_ceil(page_count)));

  const auto raw_page = pmm::alloc_pages(pmm::PageMobility::Unmovable, order);
  if (!raw_page) [[unlikely]] {
    return nullptr;
  }

  // A null cache is how kfree() recognises a page allocation.
  pmm::Page* page = pmm::phys_to_page(*raw_page);
  page->slub.cache = nullptr;

  return page_to_virt(page);
}

void kfree(void* ptr) noexcept {
  if (!ptr) [[unlikely]] {
    return;
  }

  pmm::Page* page = virt_to_page(ptr);
  if (!page) [[unlikely]] {
    utils::logger::warn("kfree: Attempted to free an unmapped or invalid pointer {}\n", ptr);
    return;
  }

  if (KmemCache* cache = page->slub.cache) [[likely]] {
    cache->free(ptr);
  } else {
    pmm::free_pages(pmm::page_to_phys(page), page->get_order());
  }
}

void* krealloc(void* ptr, const std::size_t new_size, const std::size_t align) noexcept {
  if (ptr == nullptr) {
    return kmalloc(new_size, align);
  }

  if (new_size == 0) {
    kfree(ptr);
    return nullptr;
  }

  const pmm::Page* page = virt_to_page(ptr);
  if (!page) [[unlikely]] {
    return nullptr;
  }

  const KmemCache* old_cache = page->slub.cache;
  const std::size_t old_size = old_cache ? old_cache->obj_size() : (PAGE_SIZE << page->get_order());

  if (new_size <= old_size &&
      utils::maths::is_aligned(reinterpret_cast<std::uintptr_t>(ptr), align)) [[likely]] {
    return ptr;
  }

  void* new_ptr = kmalloc(new_size, align);
  if (!new_ptr) {
    return nullptr;
  }

  klib::memcpy(new_ptr, ptr, std::min(old_size, new_size));
  kfree(ptr);
  return new_ptr;
}
}  // namespace kernel::memory::heap

using namespace kernel::memory::heap;

[[nodiscard]] void* operator new(const std::size_t size) {
  void* ptr = kmalloc(size);
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in operator new");
  }

  return ptr;
}

[[nodiscard]] void* operator new[](const std::size_t size) {
  void* ptr = kmalloc(size);
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in operator new[]");
  }

  return ptr;
}

[[nodiscard]] void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  return kmalloc(size);
}
[[nodiscard]] void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  return kmalloc(size);
}

[[nodiscard]] void* operator new(const std::size_t size, std::align_val_t alignment) {
  void* ptr = kmalloc(size, static_cast<std::size_t>(alignment));
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in aligned operator new");
  }

  return ptr;
}

[[nodiscard]] void* operator new[](const std::size_t size, std::align_val_t alignment) {
  void* ptr = kmalloc(size, static_cast<std::size_t>(alignment));
  if (!ptr) [[unlikely]] {
    kernel::utils::logger::fatal("Out of memory in aligned operator new[]");
  }

  return ptr;
}

[[nodiscard]] void* operator new(const std::size_t size, std::align_val_t alignment,
                                 const std::nothrow_t&) noexcept {
  return kmalloc(size, static_cast<std::size_t>(alignment));
}

[[nodiscard]] void* operator new[](const std::size_t size, std::align_val_t alignment,
                                   const std::nothrow_t&) noexcept {
  return kmalloc(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept { kfree(ptr); }
void operator delete[](void* ptr) noexcept { kfree(ptr); }

void operator delete(void* ptr, std::size_t /* size */) noexcept { kfree(ptr); }
void operator delete[](void* ptr, std::size_t /* size */) noexcept { kfree(ptr); }

void operator delete(void* ptr, std::align_val_t /* alignment */) noexcept { kfree(ptr); }
void operator delete[](void* ptr, std::align_val_t /* alignment */) noexcept { kfree(ptr); }

void operator delete(void* ptr, std::size_t /* size */, std::align_val_t /* alignment */) noexcept {
  kfree(ptr);
}
void operator delete[](void* ptr, std::size_t /* size */,
                       std::align_val_t /* alignment */) noexcept {
  kfree(ptr);
}
