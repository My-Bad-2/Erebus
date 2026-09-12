#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
auto AddressSpace::alloc(const std::size_t size_bytes, const AccessFlags access, const CacheMode cache,
                         const PageSize page_size, const VMAType type, const std::uint8_t pkey,
                         const VirtualAddress hint) noexcept -> std::expected<VirtualAddress, Error> {
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);
  if (hint && !hint.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  const auto vma_res = s_vma_cache->alloc();
  const auto obj_res = s_vmo_cache->alloc();
  if (!vma_res || !obj_res) {
    if (vma_res) {
      s_vma_cache->free(*vma_res);
    }

    if (obj_res) {
      s_vmo_cache->free(*obj_res);
    }

    return std::unexpected(Error::OutOfMemory);
  }

  auto vma = static_cast<VMArea *>(*vma_res);
  auto obj = static_cast<VMObject *>(*obj_res);

  new (vma) VMArea();
  new (obj) VMObject(type, aligned_size / bytes_per_page);

  utils::ExclusivePreemptGuard guard(m_as_lock);

  VirtualAddress addr = hint ? hint : find_free_gap(aligned_size, bytes_per_page);
  vma->start = addr;
  vma->end = addr + aligned_size;

  vma->flags.clear();
  vma->flags.set_access(access);
  vma->flags.set_cache(cache);
  vma->flags.set_page_size(page_size);
  vma->flags.set_type(type);
  vma->flags.set_numa(NUMAPolicy::FirstTouch);
  vma->flags.set_pkey(pkey);

  if (type == VMAType::HardwareMMIO || has_flag(access, AccessFlags::Mmio)) {
    vma->flags.set_is_wired(1);
  }

  vma->vm_object = obj;
  vma->vm_object->add_ref();
  vma->object_offset = 0;
  vma->fault_count = 0;
  vma->preferred_numa_node = hw::percpu::numa_node();

  m_vma_tree.insert(addr.value(), vma);
  link_vma(vma);

  return addr;
}

std::expected<void, Error> AddressSpace::free(const VirtualAddress addr) noexcept {
  utils::ExclusivePreemptGuard guard(m_as_lock);

  VMArea *vma = nullptr;
  if (!m_vma_tree.get(addr.value(), vma)) {
    return std::unexpected(Error::NotMapped);
  }

  unlink_vma(vma);
  m_vma_tree.erase(addr.value());

  if (vma->vm_object) {
    vma->vm_object->drop_ref();
  }

  [[maybe_unused]] auto _ = m_pagemap.unmap_range(vma->start, vma->size_bytes());
  tlb::ShootdownCoordinator::broadcast(&m_pagemap, vma->start, vma->size_bytes());

  s_vma_cache->free(vma);
  return {};
}

std::expected<VirtualAddress, Error> AddressSpace::realloc(VirtualAddress old_addr,
                                                           const std::size_t new_size) noexcept {
  utils::ExclusivePreemptGuard guard(m_as_lock);

  VMArea *vma = nullptr;
  if (!m_vma_tree.get(old_addr.value(), vma)) {
    return std::unexpected(Error::NotMapped);
  }

  const PageSize page_size = vma->flags.get_page_size();
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t new_aligned = utils::maths::align_up(new_size, bytes_per_page);
  const std::size_t old_size = vma->size_bytes();

  if (new_aligned <= old_size) {
    return old_addr;
  }

  // Expand in-place
  if (!vma->next_vma || vma->next_vma->start >= (vma->start + new_aligned)) {
    vma->end = vma->start + new_aligned;
    vma->vm_object->expand_max_pages(new_aligned / bytes_per_page);
    return old_addr;
  }

  // Move the VMA
  const VirtualAddress new_addr = find_free_gap(new_aligned, bytes_per_page);

  m_vma_tree.erase(old_addr.value());
  vma->start = new_addr;
  vma->end = new_addr + new_aligned;
  vma->vm_object->expand_max_pages(new_aligned / bytes_per_page);
  m_vma_tree.insert(new_addr.value(), vma);

  unlink_vma(vma);
  link_vma(vma);

  const auto res = m_pagemap.move_virtual_range(old_addr, new_addr, old_size);
  if (!res) {
    return std::unexpected(res.error());
  }

  tlb::ShootdownCoordinator::broadcast(&m_pagemap, old_addr, old_size);
  return new_addr;
}

auto AddressSpace::map_object(VMObject *existing_obj, std::size_t size_bytes, std::size_t obj_offset_pages,
                              AccessFlags access, CacheMode cache, PageSize page_size, VMAType type, std::uint8_t pkey,
                              VirtualAddress hint) noexcept -> std::expected<VirtualAddress, Error> {
  if (!existing_obj) {
    return std::unexpected(Error::InvalidState);
  }

  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);

  if (hint && !hint.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto vma_res = s_vma_cache->alloc();
  if (!vma_res) {
    return std::unexpected(Error::OutOfMemory);
  }

  VMArea *vma = static_cast<VMArea *>(*vma_res);
  new (vma) VMArea();

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress addr = hint ? hint : find_free_gap(aligned_size, bytes_per_page);

  vma->start = addr;
  vma->end = addr + aligned_size;

  const AccessFlags final_access = access | AccessFlags::Shared;

  vma->flags.clear();
  vma->flags.set_access(final_access);
  vma->flags.set_cache(cache);
  vma->flags.set_page_size(page_size);
  vma->flags.set_type(type);
  vma->flags.set_numa(NUMAPolicy::FirstTouch);
  vma->flags.set_pkey(pkey);
  vma->flags.set_is_wired(type == VMAType::HardwareMMIO || has_flag(final_access, AccessFlags::Mmio));

  vma->vm_object = existing_obj;
  vma->vm_object->add_ref();

  vma->object_offset = obj_offset_pages;
  vma->fault_count = 0;
  vma->preferred_numa_node = hw::percpu::numa_node();

  m_vma_tree.insert(addr.value(), vma);
  link_vma(vma);

  return addr;
}

auto AddressSpace::map_mmio(PhysicalAddress phys_base, std::size_t size_bytes, AccessFlags access, CacheMode cache,
                            PageSize page_size, std::uint8_t pkey, VirtualAddress hint) noexcept
    -> std::expected<VirtualAddress, Error> {
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);

  if (!phys_base.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (hint.value() != 0 && !hint.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto vma_res = s_vma_cache->alloc();
  if (!vma_res)
    return std::unexpected(Error::OutOfMemory);

  VMArea *vma = static_cast<VMArea *>(*vma_res);
  new (vma) VMArea();

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress addr = hint ? hint : find_free_gap(aligned_size, bytes_per_page);

  vma->start = addr;
  vma->end = addr + aligned_size;

  const AccessFlags final_access = access | AccessFlags::Mmio;

  vma->flags.clear();
  vma->flags.set_access(final_access);
  vma->flags.set_cache(cache);
  vma->flags.set_page_size(page_size);
  vma->flags.set_type(VMAType::HardwareMMIO);
  vma->flags.set_numa(NUMAPolicy::StrictLocal);
  vma->flags.set_pkey(pkey);
  vma->flags.set_is_wired(true);

  vma->vm_object = nullptr;
  vma->object_offset = 0;
  vma->fault_count = 0;
  vma->preferred_numa_node = hw::percpu::numa_node();

  for (std::size_t i = 0; i < aligned_size; i += bytes_per_page) {
    if (auto map_res = m_pagemap.map(addr + i, phys_base + i, final_access, cache, pkey, page_size); !map_res) {
      if (i > 0) {
        [[maybe_unused]] auto _ = m_pagemap.unmap_range(addr, i);
        tlb::ShootdownCoordinator::broadcast(&m_pagemap, addr, i, page_size != PageSize::Size4K);
      }

      s_vma_cache->free(vma);
      return std::unexpected(map_res.error());
    }
  }

  m_vma_tree.insert(addr.value(), vma);
  link_vma(vma);

  return addr;
}

auto AddressSpace::create_alias(const VirtualAddress src_addr, const std::size_t size_bytes,
                                const AccessFlags alias_flags) noexcept -> std::expected<VirtualAddress, Error> {
  if (!src_addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress alias_addr = find_free_gap(size_bytes, PAGE_SIZE);

  const auto vma_res = s_vma_cache->alloc();
  if (!vma_res) {
    return std::unexpected(Error::OutOfMemory);
  }

  auto vma = static_cast<VMArea *>(*vma_res);
  new (vma) VMArea();

  vma->start = alias_addr;
  vma->end = alias_addr + size_bytes;

  vma->flags.clear();
  vma->flags.set_access(alias_flags);
  vma->flags.set_type(VMAType::Anonymous);
  vma->flags.set_is_wired(true);

  vma->vm_object = nullptr;
  vma->object_offset = 0;
  vma->fault_count = 0;

  std::size_t mapped_bytes = 0;
  auto rollback = [&]() {
    if (mapped_bytes > 0) {
      [[maybe_unused]] auto _ = m_pagemap.unmap_range(alias_addr, mapped_bytes);
      tlb::ShootdownCoordinator::broadcast(&m_pagemap, alias_addr, mapped_bytes, false);
    }

    s_vma_cache->free(vma);
  };

  for (std::size_t i = 0; i < size_bytes; i += PAGE_SIZE) {
    const VirtualAddress curr_src = src_addr + i;

    auto trans = m_pagemap.translate(curr_src);
    if (!trans) {
      std::uint64_t fk;
      VMArea *src_vma = nullptr;

      if (m_vma_tree.get_floor(curr_src.value(), fk, src_vma) && curr_src >= src_vma->start &&
          curr_src < src_vma->end) {
        const auto src_flags = src_vma->flags.get_access();

        auto fault_res = resolve_vma_fault_locked(src_vma, curr_src, src_flags);
        if (!fault_res) {
          rollback();
          return std::unexpected(Error::NotMapped);
        }

        trans = m_pagemap.translate(curr_src);
      }

      if (!trans) {
        rollback();
        return std::unexpected(Error::NotMapped);
      }
    }

    const std::size_t src_page_bytes = get_page_bytes(trans->mapped_size);
    const std::size_t offset = curr_src.page_offset(src_page_bytes);
    const PhysicalAddress target_phys = trans->phys_address + offset;

    auto map_res = m_pagemap.map(alias_addr + i, target_phys, alias_flags, trans->cache, trans->pkey, PageSize::Size4K);
    if (!map_res) {
      rollback();
      return std::unexpected(map_res.error());
    }

    mapped_bytes += PAGE_SIZE;
  }

  m_vma_tree.insert(alias_addr.value(), vma);
  link_vma(vma);

  return alias_addr;
}
} // namespace kernel::memory::vmm