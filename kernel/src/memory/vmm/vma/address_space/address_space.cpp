#include "hal/percpu.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
namespace {
constexpr bool enable_aslr = true;
} // namespace

VirtualAddress AddressSpace::find_free_gap(const std::size_t size_bytes, std::size_t alignment,
                                           const bool is_heap) noexcept {
  VirtualAddress &bump = is_heap ? m_heap_bump : m_mmap_bump;
  bump = bump.align_up(alignment);

  const VirtualAddress target = bump;

  std::uint32_t rand_val = 0;
  if constexpr (enable_aslr) {
    hw::percpu::rng().get_random(rand_val);
  }

  const std::size_t entropy = (rand_val % 16) * alignment;

  bump += size_bytes + alignment + entropy;
  return target;
}

void AddressSpace::link_vma(VMArea *new_vma) noexcept {
  if (!m_vma_list_head) {
    m_vma_list_head = m_vma_list_tail = new_vma;
    new_vma->prev_vma = new_vma->next_vma = nullptr;
    return;
  }

  if (new_vma->start.value() > m_vma_list_tail->start.value()) {
    new_vma->prev_vma = m_vma_list_tail;
    new_vma->next_vma = nullptr;
    m_vma_list_tail->next_vma = new_vma;
    m_vma_list_tail = new_vma;
    return;
  }

  VMArea *curr = m_vma_list_head;
  while (curr && curr->start.value() < new_vma->start.value()) {
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

void AddressSpace::unlink_vma(VMArea *vma) noexcept {
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
}

std::expected<VMArea *, Error> AddressSpace::split_vma(VMArea *vma, const VirtualAddress split_point) noexcept {
  if (split_point <= vma->start || split_point >= vma->end) {
    return vma;
  }

  VMArea *new_vma = new VMArea();
  *new_vma = *vma;

  new_vma->start = split_point;
  vma->end = split_point;

  const PageSize page_size = static_cast<PageSize>(vma->flags.get_page_size());
  const std::size_t bytes_per_page = get_page_bytes(page_size);

  new_vma->object_offset += (split_point.value() - vma->start.value()) / bytes_per_page;

  if (new_vma->vm_object) {
    new_vma->vm_object->add_ref();
  }

  m_vma_tree.insert(new_vma->start.value(), new_vma);

  new_vma->next_vma = vma->next_vma;
  new_vma->prev_vma = vma;
  if (vma->next_vma) {
    vma->next_vma->prev_vma = new_vma;
  } else {
    m_vma_list_tail = new_vma;
  }

  return new_vma;
}

void AddressSpace::initialize() noexcept {
  const auto vma_res = heap::KmemCache::create(sizeof(VMArea), alignof(VMArea));
  const auto vmo_res = heap::KmemCache::create(sizeof(VMObject), alignof(VMObject));
  if (!vma_res || !vmo_res) {
    if (vma_res) {
      (*vma_res)->destroy();
    }

    if (vmo_res) {
      (*vmo_res)->destroy();
    }

    utils::logger::fatal("Unable to allocate memory for VMA/VMO cache!\n");
  }

  s_vmo_cache = *vmo_res;
  s_vma_cache = *vma_res;
}

AddressSpace::AddressSpace(const VirtualAddress base_addr) noexcept {
  std::uint64_t random_offsets[2] = {0};
  if constexpr (enable_aslr) {
    hw::percpu::rng().get_random(random_offsets[0]);
    hw::percpu::rng().get_random(random_offsets[1]);
  }

  auto get_entropy = [](const std::uint64_t offset, const std::uint64_t quot) {
    if constexpr (enable_aslr) {
      return (offset % quot) * PAGE_SIZE;
    } else {
      return 0;
    }
  };

  constexpr auto mmap_offset = 32ul * 1024 * PAGE_SIZE_1GB; // 32 TB offset

  const std::uint64_t heap_entropy = get_entropy(random_offsets[0], 1024);
  const std::uint64_t mmap_entropy = get_entropy(random_offsets[1], 4096);

  m_heap_bump = base_addr + heap_entropy;
  m_mmap_bump = base_addr + mmap_offset + mmap_entropy;
}

AddressSpace::~AddressSpace() noexcept {
  const VMArea *curr = m_vma_list_head;
  while (curr) {
    const VMArea *next = curr->next_vma;
    if (curr->vm_object) {
      curr->vm_object->drop_ref();
    }

    [[maybe_unused]] auto _ = m_pagemap.unmap_range(curr->start, curr->size_bytes());
    delete curr;
    curr = next;
  }
}

void AddressSpace::merge_vma(VMArea *vma) noexcept {
  if (!vma) {
    return;
  }

  auto try_merge = [this](VMArea *left, VMArea *right) -> bool {
    if (!left || !right) {
      return false;
    }

    if (left->end != right->start) {
      return false;
    }

    if (left->flags.raw() != right->flags.raw()) {
      return false;
    }

    if (left->vm_object != right->vm_object) {
      return false;
    }

    if (left->preferred_numa_node != right->preferred_numa_node) {
      return false;
    }

    const PageSize page_size = static_cast<PageSize>(left->flags.get_page_size());
    const std::size_t bytes_per_page = get_page_bytes(page_size);
    const std::size_t pages_in_left = left->size_bytes() / bytes_per_page;

    if (left->object_offset + pages_in_left != right->object_offset) {
      return false;
    }

    left->end = right->end;

    this->unlink_vma(right);
    this->m_vma_tree.erase(right->start.value());

    if (right->vm_object) {
      right->vm_object->drop_ref();
    }

    s_vma_cache->free(right);

    return true;
  };

  if (try_merge(vma, vma->next_vma)) {
    // vma absorbed next_vma.
  }

  if (try_merge(vma->prev_vma, vma)) {
    // prev_vma absorbed vma. vma is deleted!
  }
}

std::expected<void, Error> AddressSpace::clone_into(AddressSpace &child) noexcept {
  utils::ExclusivePreemptGuard parent_guard(m_as_lock);
  utils::ExclusivePreemptGuard child_guard{child.m_as_lock};

  auto rollback = [&](const VMArea *stop_point) {
    VMArea *child_curr = child.m_vma_list_head;
    while (child_curr) {
      VMArea *next = child_curr->next_vma;
      if (child_curr->vm_object) {
        child_curr->vm_object->drop_ref();
      }

      child.m_vma_tree.erase(child_curr->start.value());
      s_vma_cache->free(child_curr);
      child_curr = next;
    }

    child.m_vma_list_head = child.m_vma_list_tail = nullptr;

    VMArea *parent_curr = m_vma_list_head;
    while (parent_curr != stop_point) {
      const AccessFlags access = parent_curr->flags.get_access();

      if (has_flag(access, AccessFlags::CopyOnWrite) && parent_curr->vm_object) {
        const AccessFlags orig = access & ~AccessFlags::CopyOnWrite;
        parent_curr->flags.set_access(orig);

        VMObject *shadow = parent_curr->vm_object;
        if (VMObject *orig_obj = shadow->shadow_parent()) {
          orig_obj->add_ref();
          parent_curr->vm_object = orig_obj;
          shadow->drop_ref();
        }

        if (has_flag(orig, AccessFlags::Write)) {
          [[maybe_unused]] auto _ =
              m_pagemap.protect_virtual_range(parent_curr->start, parent_curr->size_bytes(), orig);
        }
      }

      parent_curr = parent_curr->next_vma;
    }

    tlb::ShootdownCoordinator::broadcast(&m_pagemap, VirtualAddress{}, 0, true);
  };

  VMArea *curr = m_vma_list_head;

  while (curr) {
    auto vma_res = s_vma_cache->alloc();
    if (!vma_res) {
      rollback(curr);
      return std::unexpected(Error::OutOfMemory);
    }

    VMArea *child_vma = static_cast<VMArea *>(*vma_res);
    new (child_vma) VMArea();

    *child_vma = *curr;
    child_vma->prev_vma = child_vma->next_vma = nullptr;
    child_vma->fault_count = 0;

    const AccessFlags access = curr->flags.get_access();
    const bool is_shared = has_flag(access, AccessFlags::Shared) || has_flag(access, AccessFlags::Mmio);

    if (!is_shared && curr->vm_object) {
      VMObject *original_obj = curr->vm_object;

      VMObject *parent_shadow = original_obj->create_shadow_clone();
      VMObject *child_shadow = original_obj->create_shadow_clone();

      if (!parent_shadow || !child_shadow) {
        if (parent_shadow) {
          parent_shadow->drop_ref();
        }

        if (child_shadow) {
          child_shadow->drop_ref();
        }

        s_vma_cache->free(child_vma);
        rollback(curr);
        return std::unexpected(Error::OutOfMemory);
      }

      const AccessFlags cow_access = access | AccessFlags::CopyOnWrite;

      if (has_flag(access, AccessFlags::Write)) {
        const AccessFlags hw_ro_flags = cow_access & ~AccessFlags::Write;

        if (auto res = m_pagemap.protect_virtual_range(curr->start, curr->size_bytes(), hw_ro_flags); !res) {
          parent_shadow->drop_ref();
          child_shadow->drop_ref();
          s_vma_cache->free(child_vma);
          rollback(curr);
          return std::unexpected(res.error());
        }
      }

      curr->flags.set_access(cow_access);
      child_vma->flags.set_access(cow_access);

      curr->vm_object = parent_shadow;
      child_vma->vm_object = child_shadow;
      original_obj->drop_ref();
    } else if (curr->vm_object) {
      curr->vm_object->add_ref();
    }

    child.m_vma_tree.insert(child_vma->start.value(), child_vma);
    child.link_vma(child_vma);

    curr = curr->next_vma;
  }

  tlb::ShootdownCoordinator::broadcast(&m_pagemap, VirtualAddress{}, 0, true);
  return {};
}
} // namespace kernel::memory::vmm