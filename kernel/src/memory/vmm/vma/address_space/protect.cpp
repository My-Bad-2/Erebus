#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> AddressSpace::protect(const VirtualAddress addr,
                                                 const std::size_t size_bytes,
                                                 const AccessFlags new_flags) noexcept {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size_bytes == 0) {
    return {};
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress end_addr = addr + static_cast<VirtualAddress::OffsetType>(size_bytes);
  const auto first = isolate_range(addr, end_addr);
  if (!first) {
    return std::unexpected(first.error());
  }

  VMArea* last = nullptr;
  std::expected<void, Error> result{};

  for (VMArea* vma = *first; vma && vma->start < end_addr; vma = vma->next_vma) {
    // Only the permissions change. Copy-on-write, sharing, user access and the like describe
    // what the mapping is, and a protection change must not quietly turn them off.
    const AccessFlags old_access = vma->flags.get_access();
    const AccessFlags updated = (old_access & ~PROTECTION_FLAGS) | (new_flags & PROTECTION_FLAGS);

    // Page tables first, so that a failure leaves this VMA consistent with its mappings.
    result = m_pagemap.protect_virtual_range(vma->start, vma->size_bytes(), updated);
    if (!result) [[unlikely]] {
      break;
    }

    vma->flags.set_access(updated);
    last = vma;
  }

  coalesce_range(last, addr);
  return result;
}

std::expected<void, Error> AddressSpace::lock_memory(const VirtualAddress addr,
                                                     const std::size_t size_bytes) noexcept {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size_bytes == 0) {
    return {};
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress end_addr = addr + static_cast<VirtualAddress::OffsetType>(size_bytes);
  const auto first = isolate_range(addr, end_addr);
  if (!first) {
    return std::unexpected(first.error());
  }

  VMArea* last = nullptr;
  std::expected<void, Error> result{};

  for (VMArea* vma = *first; vma && vma->start < end_addr && result; vma = vma->next_vma) {
    vma->flags.set_is_locked(true);
    last = vma;

    // Wired VMAs are fully mapped already.
    if (!vma->vm_object) {
      continue;
    }

    const AccessFlags fault_type = has_flag(vma->flags.get_access(), AccessFlags::Write)
                                       ? AccessFlags::Write
                                       : AccessFlags::Read;
    const std::size_t bpp = vma->page_bytes();

    for (VirtualAddress page = vma->start; page < vma->end;
         page += static_cast<VirtualAddress::OffsetType>(bpp)) {
      if (!m_pagemap.translate(page)) {
        result = resolve_vma_fault_locked(vma, page, fault_type);
        if (!result) [[unlikely]] {
          break;
        }
      }
    }
  }

  coalesce_range(last, addr);
  return result;
}

auto AddressSpace::unlock_memory(const VirtualAddress addr, const std::size_t size_bytes) noexcept
    -> std::expected<void, Error> {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  if (size_bytes == 0) {
    return {};
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  const VirtualAddress end_addr = addr + static_cast<VirtualAddress::OffsetType>(size_bytes);
  const auto first = isolate_range(addr, end_addr);
  if (!first) {
    return std::unexpected(first.error());
  }

  VMArea* last = nullptr;
  for (VMArea* vma = *first; vma && vma->start < end_addr; vma = vma->next_vma) {
    vma->flags.set_is_locked(false);
    last = vma;
  }

  coalesce_range(last, addr);
  return {};
}

auto AddressSpace::evict_page(const VirtualAddress addr) noexcept -> std::expected<void, Error> {
  utils::ExclusivePreemptGuard guard(m_as_lock);

  VMArea* vma = find_vma(addr);
  if (!vma) {
    return std::unexpected(Error::NotMapped);
  }

  if (vma->flags.get_is_wired() || vma->flags.get_is_locked()) {
    return std::unexpected(Error::SecurityViolation);
  }

  const std::size_t bytes_per_page = vma->page_bytes();
  return m_pagemap.unmap_range(addr.align_down(bytes_per_page), bytes_per_page);
}
}  // namespace kernel::memory::vmm
