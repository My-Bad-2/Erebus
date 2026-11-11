#include "hal/smp/percpu.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
namespace {
void init_vma(VMArea* vma, const VirtualAddress addr, const std::size_t size_bytes,
              const AccessFlags access, const CacheMode cache, const PageSize page_size,
              const VMAType type, const NUMAPolicy numa, const std::uint8_t pkey) noexcept {
  vma->start = addr;
  vma->end = addr + static_cast<VirtualAddress::OffsetType>(size_bytes);

  vma->flags.clear();
  vma->flags.set_access(access);
  vma->flags.set_cache(cache);
  vma->flags.set_page_size(page_size);
  vma->flags.set_type(type);
  vma->flags.set_numa(numa);
  vma->flags.set_pkey(pkey);

  vma->vm_object = nullptr;
  vma->object_offset = 0;
  vma->fault_count = 0;
  vma->preferred_numa_node = hw::percpu::numa_node_id();
}
}  // namespace

auto AddressSpace::alloc(const std::size_t size_bytes, const AccessFlags access,
                         const CacheMode cache, const PageSize page_size, const VMAType type,
                         const std::uint8_t pkey, const VirtualAddress hint) noexcept
    -> std::expected<VirtualAddress, Error> {
  if (size_bytes == 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);
  if (aligned_size < size_bytes) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  if (hint && !hint.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  // All the range may come to hold counts from the start, before anything is allocated for it.
  // Device memory holds no pages of its own.
  const bool is_wired = type == VMAType::HardwareMMIO || has_flag(access, AccessFlags::Mmio);
  auto commit = QuotaCharge::take(is_wired ? nullptr : m_quota.get(), aligned_size);
  if (!commit) [[unlikely]] {
    return std::unexpected(commit.error());
  }

  // Object pages are indexed from the start of the VMA, and a stack grows down. Leave enough
  // indices below the initial range for it to reach its maximum size.
  const bool is_stack = has_flag(access, AccessFlags::Stack);
  const std::size_t headroom_pages = is_stack ? MAX_STACK_BYTES / bytes_per_page : 0;

  // A user stack also gets the addresses it may grow into, as nothing else placed there could
  // ever move out of its way. Kernel stacks are not meant to grow at all.
  const std::size_t growth_gap =
      is_stack && has_flag(access, AccessFlags::User) && aligned_size < MAX_STACK_BYTES
          ? MAX_STACK_BYTES - aligned_size
          : 0;

  VMArea* vma = allocate_vma();
  VMObject* obj = VMObject::create(type, page_size, headroom_pages + aligned_size / bytes_per_page);
  if (!vma || !obj) [[unlikely]] {
    if (vma) {
      release_vma(vma);
    }

    if (obj) {
      obj->drop_ref();
    }

    return std::unexpected(Error::OutOfMemory);
  }

  WriteGuard guard{*this};

  VirtualAddress addr = hint;
  Error failure = Error::Success;
  if (hint && !range_is_free(hint, hint + static_cast<VirtualAddress::OffsetType>(aligned_size))) {
    failure = Error::AlreadyMapped;
  } else if (!hint && !((addr = find_free_gap(aligned_size + growth_gap, bytes_per_page)))) {
    failure = Error::OutOfMemory;
  }

  if (failure != Error::Success) [[unlikely]] {
    release_vma(vma);
    obj->drop_ref();
    return std::unexpected(failure);
  }

  if (!hint) {
    addr += static_cast<VirtualAddress::OffsetType>(growth_gap);
  }

  init_vma(vma, addr, aligned_size, access, cache, page_size, type, NUMAPolicy::FirstTouch, pkey);
  vma->flags.set_is_wired(is_wired);
  obj->adopt_commit(std::move(*commit));
  vma->vm_object = obj;  // the creation reference now belongs to the VMA
  vma->object_offset = headroom_pages;

  if (!m_vma_tree.insert(addr.value(), vma)) [[unlikely]] {
    release_vma(vma);
    obj->drop_ref();
    return std::unexpected(Error::OutOfMemory);
  }

  link_vma(vma);

  if (has_flag(access, AccessFlags::Populate)) {
    const AccessFlags fault_type =
        has_flag(access, AccessFlags::Write) ? AccessFlags::Write : AccessFlags::Read;

    for (std::size_t i = 0; i < aligned_size; i += bytes_per_page) {
      const auto fault_res = resolve_vma_fault_locked(
          vma, addr + static_cast<VirtualAddress::OffsetType>(i), fault_type);
      if (!fault_res) [[unlikely]] {
        [[maybe_unused]] auto _ = m_pagemap.unmap_range(addr, i);
        unlink_vma(vma);
        m_vma_tree.erase(addr.value());
        obj->drop_ref();
        release_vma(vma);
        return std::unexpected(fault_res.error());
      }
    }
  }

  return addr;
}

std::expected<void, Error> AddressSpace::free(const VirtualAddress addr) noexcept {
  WriteGuard guard{*this};

  VMArea* vma = nullptr;
  if (!m_vma_tree.get(addr.value(), vma)) {
    return std::unexpected(Error::NotMapped);
  }

  release_vma_locked(vma);
  return {};
}

std::expected<void, Error> AddressSpace::free_containing(const VirtualAddress addr) noexcept {
  WriteGuard guard{*this};

  VMArea* vma = find_vma(addr);
  if (vma == nullptr) {
    return std::unexpected(Error::NotMapped);
  }

  release_vma_locked(vma);
  return {};
}

void AddressSpace::release_vma_locked(VMArea* vma) noexcept {
  const VirtualAddress addr = vma->start;
  unlink_vma(vma);
  m_vma_tree.erase(addr.value());

  // unmap_range() flushes the TLBs, so the pages are unreachable before the object can free them.
  [[maybe_unused]] auto _ = m_pagemap.unmap_range(vma->start, vma->size_bytes());
  if (vma->vm_object) {
    vma->vm_object->drop_ref();
  }

  release_vma(vma);
}

std::expected<VirtualAddress, Error> AddressSpace::realloc(const VirtualAddress old_addr,
                                                           const std::size_t new_size) noexcept {
  WriteGuard guard{*this};

  VMArea* vma = nullptr;
  if (!m_vma_tree.get(old_addr.value(), vma)) {
    return std::unexpected(Error::NotMapped);
  }

  if (!vma->vm_object || vma->flags.get_is_wired()) {
    return std::unexpected(Error::InvalidState);
  }

  const std::size_t bytes_per_page = vma->page_bytes();
  const std::size_t new_aligned = utils::maths::align_up(new_size, bytes_per_page);
  const std::size_t old_size = vma->size_bytes();

  if (new_aligned <= old_size) {
    return old_addr;
  }

  const std::size_t needed_pages = vma->object_offset + new_aligned / bytes_per_page;
  const auto grow_end = vma->start + static_cast<VirtualAddress::OffsetType>(new_aligned);

  // What the object may come to hold grows with it, and counts against what it did before.
  const std::size_t added = new_aligned - old_size;
  if (const auto charged = vma->vm_object->commit_more(added); !charged) [[unlikely]] {
    return std::unexpected(charged.error());
  }

  if (range_is_free(vma->end, grow_end)) {
    vma->end = grow_end;
    vma->vm_object->expand_max_pages(needed_pages);
    return old_addr;
  }

  const VirtualAddress new_addr = find_free_gap(new_aligned, bytes_per_page);
  if (!new_addr) [[unlikely]] {
    vma->vm_object->commit_less(added);
    return std::unexpected(Error::OutOfMemory);
  }

  // Claim the new key first: it is the only step that can fail.
  if (!m_vma_tree.insert(new_addr.value(), vma)) [[unlikely]] {
    vma->vm_object->commit_less(added);
    return std::unexpected(Error::OutOfMemory);
  }

  // The object keeps the pages, so dropping the old translations is all a move needs. They
  // fault back in at the new address.
  [[maybe_unused]] auto _ = m_pagemap.unmap_range(old_addr, old_size);

  unlink_vma(vma);
  m_vma_tree.erase(old_addr.value());

  vma->start = new_addr;
  vma->end = new_addr + static_cast<VirtualAddress::OffsetType>(new_aligned);
  vma->vm_object->expand_max_pages(needed_pages);
  link_vma(vma);

  return new_addr;
}

auto AddressSpace::map_object(VMObject* existing_obj, const std::size_t size_bytes,
                              const std::size_t obj_offset_pages, const AccessFlags access,
                              const CacheMode cache, const PageSize page_size, const VMAType type,
                              const std::uint8_t pkey, const VirtualAddress hint) noexcept
    -> std::expected<VirtualAddress, Error> {
  if (!existing_obj || size_bytes == 0) {
    return std::unexpected(Error::InvalidArgument);
  }

  // Offsets into the object are in its own page size.
  if (existing_obj->page_size() != page_size) {
    return std::unexpected(Error::InvalidArgument);
  }

  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);

  if (hint && !hint.is_aligned(bytes_per_page)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  VMArea* vma = allocate_vma();
  if (!vma) {
    return std::unexpected(Error::OutOfMemory);
  }

  WriteGuard guard{*this};

  VirtualAddress addr = hint;
  if (hint ? !range_is_free(hint, hint + static_cast<VirtualAddress::OffsetType>(aligned_size))
           : !((addr = find_free_gap(aligned_size, bytes_per_page)))) [[unlikely]] {
    release_vma(vma);
    return std::unexpected(hint ? Error::AlreadyMapped : Error::OutOfMemory);
  }

  const AccessFlags final_access = access | AccessFlags::Shared;
  init_vma(vma, addr, aligned_size, final_access, cache, page_size, type, NUMAPolicy::FirstTouch,
           pkey);
  vma->flags.set_is_wired(type == VMAType::HardwareMMIO ||
                          has_flag(final_access, AccessFlags::Mmio));

  if (!m_vma_tree.insert(addr.value(), vma)) [[unlikely]] {
    release_vma(vma);
    return std::unexpected(Error::OutOfMemory);
  }

  existing_obj->add_ref();
  vma->vm_object = existing_obj;
  vma->object_offset = obj_offset_pages;
  link_vma(vma);

  return addr;
}

auto AddressSpace::map_mmio(const PhysicalAddress phys_base, const std::size_t size_bytes,
                            const AccessFlags access, const CacheMode cache,
                            const PageSize page_size, const std::uint8_t pkey,
                            const VirtualAddress hint) noexcept
    -> std::expected<VirtualAddress, Error> {
  if (size_bytes == 0) {
    return std::unexpected(Error::InvalidArgument);
  }

  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::size_t aligned_size = utils::maths::align_up(size_bytes, bytes_per_page);

  if (!phys_base.is_aligned(bytes_per_page) || (hint && !hint.is_aligned(bytes_per_page))) {
    return std::unexpected(Error::InvalidAlignment);
  }

  VMArea* vma = allocate_vma();
  if (!vma) {
    return std::unexpected(Error::OutOfMemory);
  }

  WriteGuard guard{*this};

  VirtualAddress addr = hint;
  if (hint ? !range_is_free(hint, hint + static_cast<VirtualAddress::OffsetType>(aligned_size))
           : !((addr = find_free_gap(aligned_size, bytes_per_page)))) [[unlikely]] {
    release_vma(vma);
    return std::unexpected(hint ? Error::AlreadyMapped : Error::OutOfMemory);
  }

  const AccessFlags final_access = access | AccessFlags::Mmio;
  init_vma(vma, addr, aligned_size, final_access, cache, page_size, VMAType::HardwareMMIO,
           NUMAPolicy::StrictLocal, pkey);
  vma->flags.set_is_wired(true);

  for (std::size_t i = 0; i < aligned_size; i += bytes_per_page) {
    const auto offset = static_cast<VirtualAddress::OffsetType>(i);
    if (auto map_res =
            m_pagemap.map(addr + offset, phys_base + offset, final_access, cache, pkey, page_size);
        !map_res) [[unlikely]] {
      [[maybe_unused]] auto _ = m_pagemap.unmap_range(addr, i);
      release_vma(vma);
      return std::unexpected(map_res.error());
    }
  }

  if (!m_vma_tree.insert(addr.value(), vma)) [[unlikely]] {
    [[maybe_unused]] auto _ = m_pagemap.unmap_range(addr, aligned_size);
    release_vma(vma);
    return std::unexpected(Error::OutOfMemory);
  }

  link_vma(vma);

  return addr;
}

auto AddressSpace::create_alias(const VirtualAddress src_addr, const std::size_t size_bytes,
                                const AccessFlags alias_flags) noexcept
    -> std::expected<VirtualAddress, Error> {
  if (!src_addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size_bytes == 0) {
    return std::unexpected(Error::InvalidArgument);
  }

  VMArea* vma = allocate_vma();
  if (!vma) {
    return std::unexpected(Error::OutOfMemory);
  }

  WriteGuard guard{*this};

  const VirtualAddress alias_addr = find_free_gap(size_bytes, PAGE_SIZE);
  if (!alias_addr) [[unlikely]] {
    release_vma(vma);
    return std::unexpected(Error::OutOfMemory);
  }

  init_vma(vma, alias_addr, size_bytes, alias_flags, CacheMode::WriteBack, PageSize::Size4K,
           VMAType::Anonymous, NUMAPolicy::FirstTouch, 0);
  vma->flags.set_is_wired(true);

  const bool wants_write = has_flag(alias_flags, AccessFlags::Write);
  std::size_t mapped_bytes = 0;

  const auto fail = [&](const Error err) -> std::expected<VirtualAddress, Error> {
    [[maybe_unused]] auto _ = m_pagemap.unmap_range(alias_addr, mapped_bytes);
    release_vma(vma);
    return std::unexpected(err);
  };

  for (std::size_t i = 0; i < size_bytes; i += PAGE_SIZE) {
    const VirtualAddress curr_src = src_addr + static_cast<VirtualAddress::OffsetType>(i);
    auto trans = m_pagemap.translate(curr_src);

    // Fault the source in first if it is demand paged. A writable alias of copy-on-write memory
    // must see the private copy, or writes through it would land in a shared page.
    const bool needs_cow_break =
        trans && wants_write && has_flag(trans->flags, AccessFlags::CopyOnWrite);
    if (!trans || needs_cow_break) {
      VMArea* src_vma = find_vma(curr_src);
      if (!src_vma || !src_vma->vm_object) {
        return fail(Error::NotMapped);
      }

      const bool src_writable = has_flag(src_vma->flags.get_access(), AccessFlags::Write);
      const AccessFlags fault_type =
          (wants_write && src_writable) ? AccessFlags::Write : AccessFlags::Read;
      if (const auto fault_res = resolve_vma_fault_locked(src_vma, curr_src, fault_type);
          !fault_res) {
        return fail(fault_res.error());
      }

      trans = m_pagemap.translate(curr_src);
      if (!trans) [[unlikely]] {
        return fail(Error::NotMapped);
      }
    }

    // translate() already points at the exact 4 KiB frame, even inside a huge page.
    if (auto map_res = m_pagemap.map(alias_addr + static_cast<VirtualAddress::OffsetType>(i),
                                     trans->phys_address, alias_flags, trans->cache, trans->pkey,
                                     PageSize::Size4K);
        !map_res) {
      return fail(map_res.error());
    }

    mapped_bytes += PAGE_SIZE;
  }

  if (!m_vma_tree.insert(alias_addr.value(), vma)) [[unlikely]] {
    return fail(Error::OutOfMemory);
  }

  link_vma(vma);

  return alias_addr;
}
}  // namespace kernel::memory::vmm
