#include "crypto/random.hpp"
#include "hal/smp/percpu.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/assert.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::memory::vmm {
namespace {
constexpr bool enable_aslr = true;
}  // namespace

AddressSpace::WriteGuard::WriteGuard(AddressSpace& space) noexcept : m_space(space) {
  // The grace period below would wait for the caller's own read section. Interrupt handlers are
  // in one, so they cannot change an address space either.
  KASSERT(!hw::percpu::in_rcu_read());

  hw::percpu::preempt_disable();
  m_space.m_writers.lock();

  // Faults that found the fast path open may be anywhere in the tree and the VMAs, or halfway
  // through mapping a page the way a VMA said before this changes it. Closing it keeps new ones
  // out, and the grace period waits for the ones that got in. synchronize() fences before it
  // looks at the readers, so a fault either sees it closed or is waited for.
  if (m_space.m_fast_faults.load(std::memory_order_relaxed)) {
    m_space.m_fast_faults.store(false, std::memory_order_relaxed);
    utils::rcu::synchronize();
  }

  m_space.m_as_lock.write_lock();
}

AddressSpace::WriteGuard::~WriteGuard() noexcept {
  m_space.m_as_lock.write_unlock();
  m_space.m_writers.unlock();
  hw::percpu::preempt_enable();
}

void AddressSpace::open_fast_faults() noexcept {
  // A writer that holds m_writers has closed it, or found it closed, and goes on as if it were, so
  // it stays closed for that one. A writer that takes m_writers after this finds it open and
  // waits for the faults that got in. What writers did before is in place: their unlock, and the
  // release here, come before any fault that finds it open.
  if (!m_fast_faults.load(std::memory_order_relaxed) && m_writers.try_lock()) {
    m_fast_faults.store(true, std::memory_order_release);
    m_writers.unlock();
  }
}

VMArea* AddressSpace::allocate_vma() noexcept {
  const auto mem = s_vma_cache->alloc();
  return mem ? new (*mem) VMArea() : nullptr;
}

void AddressSpace::release_vma(VMArea* vma) noexcept { s_vma_cache->free(vma); }

VMArea* AddressSpace::find_vma(const VirtualAddress addr) const noexcept {
  std::uint64_t key;
  VMArea* vma = nullptr;
  if (m_vma_tree.get_floor(addr.value(), key, vma) && vma->contains(addr)) {
    return vma;
  }

  return nullptr;
}

bool AddressSpace::range_is_free(const VirtualAddress start,
                                 const VirtualAddress end) const noexcept {
  if (end <= start) {
    return false;
  }

  // VMAs do not overlap, so the last one starting before `end` is the only one that can reach
  // into the range.
  std::uint64_t key;
  VMArea* vma = nullptr;
  return !m_vma_tree.get_floor(end.value() - 1, key, vma) || vma->end <= start;
}

VirtualAddress AddressSpace::first_fit(const VirtualAddress from, const VirtualAddress limit,
                                       const std::size_t size_bytes,
                                       const std::size_t alignment) const noexcept {
  VirtualAddress candidate = from.align_up(alignment);

  while (true) {
    const VirtualAddress end = candidate + static_cast<VirtualAddress::OffsetType>(size_bytes);
    if (end <= candidate || end > limit) {
      return VirtualAddress{};
    }

    // Only the last VMA starting below `end` can overlap. If it does, the hole in front of it
    // is too small by construction, so skip past it, keeping an unmapped guard gap.
    std::uint64_t key;
    VMArea* blocker = nullptr;
    if (!m_vma_tree.get_floor(end.value() - 1, key, blocker) || blocker->end <= candidate) {
      return candidate;
    }

    candidate =
        (blocker->end + static_cast<VirtualAddress::OffsetType>(alignment)).align_up(alignment);
  }
}

VirtualAddress AddressSpace::find_free_gap(const std::size_t size_bytes,
                                           const std::size_t alignment,
                                           const bool is_heap) noexcept {
  Region& region = is_heap ? m_heap_region : m_mmap_region;

  // Fresh addresses first, then wrap around and reuse the first hole that fits.
  VirtualAddress found = first_fit(region.bump, region.limit, size_bytes, alignment);
  if (!found) {
    found = first_fit(region.start, region.limit, size_bytes, alignment);
    if (!found) [[unlikely]] {
      return VirtualAddress{};
    }
  }

  // Leave an unmapped guard gap after the range, plus a little randomness for the next one.
  const std::size_t entropy = enable_aslr ? crypto::random::below(16) * alignment : 0;
  const VirtualAddress next =
      found + static_cast<VirtualAddress::OffsetType>(size_bytes + alignment + entropy);
  region.bump = (next > found && next < region.limit) ? next : region.limit;
  return found;
}

void AddressSpace::link_vma(VMArea* new_vma) noexcept {
  if (!m_vma_list_head) {
    m_vma_list_head = m_vma_list_tail = new_vma;
    new_vma->prev_vma = new_vma->next_vma = nullptr;
    return;
  }

  if (new_vma->start > m_vma_list_tail->start) {
    new_vma->prev_vma = m_vma_list_tail;
    new_vma->next_vma = nullptr;
    m_vma_list_tail->next_vma = new_vma;
    m_vma_list_tail = new_vma;
    return;
  }

  VMArea* curr = m_vma_list_head;
  while (curr && curr->start < new_vma->start) {
    curr = curr->next_vma;
  }

  new_vma->next_vma = curr;
  new_vma->prev_vma = curr ? curr->prev_vma : m_vma_list_tail;

  if (new_vma->prev_vma) {
    new_vma->prev_vma->next_vma = new_vma;
  } else {
    m_vma_list_head = new_vma;
  }

  if (curr) {
    curr->prev_vma = new_vma;
  }
}

void AddressSpace::unlink_vma(VMArea* vma) noexcept {
  if (vma->prev_vma) {
    vma->prev_vma->next_vma = vma->next_vma;
  } else {
    m_vma_list_head = vma->next_vma;
  }

  if (vma->next_vma) {
    vma->next_vma->prev_vma = vma->prev_vma;
  } else {
    m_vma_list_tail = vma->prev_vma;
  }

  vma->prev_vma = vma->next_vma = nullptr;
}

std::expected<VMArea*, Error> AddressSpace::split_vma(VMArea* vma,
                                                      const VirtualAddress split_point) noexcept {
  if (split_point <= vma->start || split_point >= vma->end) {
    return vma;
  }

  VMArea* upper = allocate_vma();
  if (!upper) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  *upper = *vma;
  upper->start = split_point;
  upper->object_offset += (split_point - vma->start) / vma->page_bytes();
  vma->end = split_point;

  if (!m_vma_tree.insert(upper->start.value(), upper)) [[unlikely]] {
    vma->end = upper->end;
    release_vma(upper);
    return std::unexpected(Error::OutOfMemory);
  }

  if (upper->vm_object) {
    upper->vm_object->add_ref();
  }

  upper->prev_vma = vma;
  upper->next_vma = vma->next_vma;
  if (vma->next_vma) {
    vma->next_vma->prev_vma = upper;
  } else {
    m_vma_list_tail = upper;
  }
  vma->next_vma = upper;

  return upper;
}

std::expected<VMArea*, Error> AddressSpace::isolate_range(const VirtualAddress start,
                                                          const VirtualAddress end) noexcept {
  VMArea* first = find_vma(start);
  if (!first) {
    return std::unexpected(Error::NotMapped);
  }

  // The whole range must be covered, without holes, and both ends have to fall on a page
  // boundary of the VMAs they cut through. Check everything before changing anything.
  VirtualAddress covered = start;
  for (const VMArea* vma = first; covered < end; vma = vma->next_vma) {
    if (!vma || vma->start > covered) {
      return std::unexpected(Error::NotMapped);
    }

    const std::size_t bpp = vma->page_bytes();
    const VirtualAddress chunk_end = std::min(vma->end, end);
    if (!covered.is_aligned(bpp) || !chunk_end.is_aligned(bpp)) {
      return std::unexpected(Error::InvalidAlignment);
    }

    covered = chunk_end;
  }

  if (first->start < start) {
    const auto upper = split_vma(first, start);
    if (!upper) [[unlikely]] {
      return std::unexpected(upper.error());
    }

    first = *upper;
  }

  if (VMArea* last = find_vma(end - 1); last->end > end) {
    if (const auto res = split_vma(last, end); !res) [[unlikely]] {
      return std::unexpected(res.error());
    }
  }

  return first;
}

bool AddressSpace::can_merge(const VMArea* left, const VMArea* right) noexcept {
  // Wired VMAs have no object to keep the offsets consistent, and their physical backing need
  // not be contiguous, so they are never merged.
  if (!left || !right || !left->vm_object || left->vm_object != right->vm_object) {
    return false;
  }

  return left->end == right->start && left->flags.raw() == right->flags.raw() &&
         left->preferred_numa_node == right->preferred_numa_node &&
         left->object_offset + (left->size_bytes() / left->page_bytes()) == right->object_offset;
}

void AddressSpace::absorb_next(VMArea* left) noexcept {
  VMArea* right = left->next_vma;
  left->end = right->end;

  unlink_vma(right);
  m_vma_tree.erase(right->start.value());

  if (right->vm_object) {
    right->vm_object->drop_ref();
  }

  release_vma(right);
}

void AddressSpace::coalesce_range(VMArea* last, const VirtualAddress start) noexcept {
  if (!last) {
    return;
  }

  if (can_merge(last, last->next_vma)) {
    absorb_next(last);
  }

  // Walk back over the range, folding each VMA into its predecessor where possible. The one in
  // front of the range is included, which is why the loop stops only once it is left behind.
  for (VMArea* curr = last; curr && curr->start >= start;) {
    VMArea* prev = curr->prev_vma;
    if (can_merge(prev, curr)) {
      absorb_next(prev);
    }

    curr = prev;
  }
}

void AddressSpace::initialize() noexcept {
  const auto vma_res = heap::KmemCache::create(sizeof(VMArea), alignof(VMArea));
  if (!vma_res) {
    utils::logger::fatal("Unable to allocate memory for VMA cache!\n");
  }

  s_vma_cache = *vma_res;
  VMObject::initialize_cache();
}

AddressSpace::AddressSpace(const VirtualAddress base_addr) noexcept {
  // A random number of pages, below `limit`, to slide a region by.
  const auto get_entropy = [](const std::uint64_t limit) -> std::uint64_t {
    return enable_aslr ? crypto::random::below(limit) * PAGE_SIZE : 0;
  };

  constexpr std::size_t REGION_BYTES = 32ul * 1024 * PAGE_SIZE_1GB;  // 32 TiB each

  // Saturate instead of wrapping if the caller put the base near the top of the address space.
  const auto offset = [base_addr](const std::size_t bytes) {
    const VirtualAddress addr = base_addr + static_cast<VirtualAddress::OffsetType>(bytes);
    return addr < base_addr ? VirtualAddress{~std::uintptr_t{0}} : addr;
  };

  const VirtualAddress heap_start = offset(get_entropy(1024));
  const VirtualAddress mmap_start = offset(REGION_BYTES + get_entropy(4096));

  m_heap_region = {.start = heap_start, .limit = offset(REGION_BYTES), .bump = heap_start};
  m_mmap_region = {.start = mmap_start, .limit = offset(2 * REGION_BYTES), .bump = mmap_start};
}

AddressSpace::~AddressSpace() noexcept {
  VMArea* curr = m_vma_list_head;
  while (curr) {
    VMArea* next = curr->next_vma;

    // Unmap first: dropping the object may free its pages, and nothing may still map them.
    [[maybe_unused]] auto _ = m_pagemap.unmap_range(curr->start, curr->size_bytes());
    if (curr->vm_object) {
      curr->vm_object->drop_ref();
    }

    release_vma(curr);
    curr = next;
  }
}

std::expected<void, Error> AddressSpace::clone_into(AddressSpace& child) noexcept {
  // The child first: it is new, so nothing can be faulting in it, and its guard waits for no one.
  // The parent's grace period then holds up no one who wants the child.
  WriteGuard child_guard{child};
  WriteGuard parent_guard{*this};

  std::size_t vma_count = 0;
  for (const VMArea* vma = m_vma_list_head; vma; vma = vma->next_vma) {
    ++vma_count;
  }

  if (vma_count == 0) {
    return {};
  }

  // Phase 1: allocate everything the clone needs while leaving the parent untouched. The new
  // parent-side shadows are parked here, by VMA index, until the commit.
  auto* parent_shadows = static_cast<VMObject**>(heap::kmalloc(sizeof(VMObject*) * vma_count));
  if (!parent_shadows) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  std::fill_n(parent_shadows, vma_count, nullptr);

  const auto abort = [&]() -> std::expected<void, Error> {
    for (VMArea* c = child.m_vma_list_head; c;) {
      VMArea* next = c->next_vma;
      [[maybe_unused]] auto _ = child.m_pagemap.unmap_range(c->start, c->size_bytes());
      child.m_vma_tree.erase(c->start.value());
      if (c->vm_object) {
        c->vm_object->drop_ref();
      }

      release_vma(c);
      c = next;
    }

    child.m_vma_list_head = child.m_vma_list_tail = nullptr;

    for (std::size_t i = 0; i < vma_count; ++i) {
      if (parent_shadows[i]) {
        parent_shadows[i]->drop_ref();
      }
    }

    heap::kfree(parent_shadows);
    return std::unexpected(Error::OutOfMemory);
  };

  std::size_t idx = 0;
  for (const VMArea* curr = m_vma_list_head; curr; curr = curr->next_vma, ++idx) {
    VMArea* child_vma = allocate_vma();
    if (!child_vma) [[unlikely]] {
      return abort();
    }

    *child_vma = *curr;
    child_vma->vm_object = nullptr;  // set below, together with the reference it needs
    child_vma->fault_count = 0;

    if (!child.m_vma_tree.insert(child_vma->start.value(), child_vma)) [[unlikely]] {
      release_vma(child_vma);
      return abort();
    }

    child.link_vma(child_vma);

    // Wired VMAs (MMIO, aliases) have no object to fault from, so the child gets the same
    // translations right now. They point at the same frames, which is what sharing a device
    // mapping means.
    if (!curr->vm_object) {
      const std::size_t bpp = curr->page_bytes();
      for (VirtualAddress page = curr->start; page < curr->end;
           page += static_cast<VirtualAddress::OffsetType>(bpp)) {
        const auto trans = m_pagemap.translate(page);
        if (!trans) {
          continue;
        }

        if (!child.m_pagemap.map(page, trans->phys_address, curr->flags.get_access(), trans->cache,
                                 trans->pkey, curr->flags.get_page_size())) {
          return abort();
        }
      }

      continue;
    }

    const AccessFlags access = curr->flags.get_access();
    if (has_flag(access, AccessFlags::Shared) || has_flag(access, AccessFlags::Mmio)) {
      curr->vm_object->add_ref();
      child_vma->vm_object = curr->vm_object;
      continue;
    }

    VMObject* child_shadow = curr->vm_object->create_shadow_clone();
    if (!child_shadow) [[unlikely]] {
      return abort();
    }

    child_vma->vm_object = child_shadow;
    child_vma->flags.set_access(access | AccessFlags::CopyOnWrite);

    parent_shadows[idx] = curr->vm_object->create_shadow_clone();
    if (!parent_shadows[idx]) [[unlikely]] {
      return abort();
    }
  }

  // Phase 2: commit. Nothing below can fail. The original object becomes read-only for both
  // sides, so every writable translation of it in the parent has to go.
  idx = 0;
  for (VMArea* curr = m_vma_list_head; curr; curr = curr->next_vma, ++idx) {
    VMObject* parent_shadow = parent_shadows[idx];
    if (!parent_shadow) {
      continue;
    }

    m_pagemap.write_protect_range(curr->start, curr->size_bytes());

    // The VMA's reference to the original is handed over to the shadow, which took its own.
    VMObject* original = curr->vm_object;
    curr->vm_object = parent_shadow;
    curr->flags.set_access(curr->flags.get_access() | AccessFlags::CopyOnWrite);
    original->drop_ref();
  }

  heap::kfree(parent_shadows);
  return {};
}
}  // namespace kernel::memory::vmm
