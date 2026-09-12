#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
std::expected<void, Error> AddressSpace::protect(const VirtualAddress addr, const std::size_t size_bytes,
                                                 const AccessFlags new_flags) noexcept {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  std::uint64_t floor_key;
  VMArea *first_vma = nullptr;
  if (!m_vma_tree.get_floor(addr.value(), floor_key, first_vma) || addr < first_vma->start) {
    return std::unexpected(Error::NotMapped);
  }

  const VirtualAddress end_addr = addr + size_bytes;
  VMArea *validate_vma = first_vma;
  VirtualAddress validate_addr = addr;

  while (validate_addr < end_addr) {
    if (!validate_vma || validate_vma->start > validate_addr) {
      return std::unexpected(Error::NotMapped);
    }

    const auto p_size = validate_vma->flags.get_page_size();
    const std::size_t bpp = get_page_bytes(p_size);
    const auto chunk_end = VirtualAddress{std::min(validate_vma->end.value(), end_addr.value())};

    if (!validate_addr.is_aligned(bpp) || !chunk_end.is_aligned(bpp)) {
      return std::unexpected(Error::InvalidAlignment);
    }

    validate_addr = chunk_end;
    validate_vma = validate_vma->next_vma;
  }

  VMArea *curr_vma = first_vma;
  VirtualAddress curr_addr = addr;

  while (curr_addr < end_addr) {
    // Split Head
    if (curr_addr > curr_vma->start) {
      auto res = split_vma(curr_vma, curr_addr);
      if (!res) {
        return std::unexpected(res.error());
      }

      curr_vma = *res;
    }

    const auto chunk_end = VirtualAddress{std::min(curr_vma->end.value(), end_addr.value())};

    // Split Tail
    if (chunk_end < curr_vma->end) {
      auto res = split_vma(curr_vma, chunk_end);
      if (!res) {
        return std::unexpected(res.error());
      }
    }

    curr_vma->flags.set_access(new_flags);

    const std::size_t chunk_size = chunk_end.value() - curr_addr.value();
    if (auto res = m_pagemap.protect_virtual_range(curr_addr, chunk_size, new_flags); !res) {
      return std::unexpected(res.error());
    }

    curr_addr = chunk_end;
    curr_vma = curr_vma->next_vma;
    merge_vma(curr_vma);
  }

  return {};
}

std::expected<void, Error> AddressSpace::lock_memory(VirtualAddress addr, std::size_t size_bytes) noexcept {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  std::uint64_t key;
  VMArea *first_vma = nullptr;
  if (!m_vma_tree.get_floor(addr.value(), key, first_vma) || addr < first_vma->start || addr >= first_vma->end) {
    return std::unexpected(Error::NotMapped);
  }

  const VirtualAddress end_addr = addr + size_bytes;
  const VMArea *validate_vma = first_vma;
  VirtualAddress validate_addr = addr;
  while (validate_addr < end_addr) {
    if (!validate_vma || validate_vma->start > validate_addr) {
      return std::unexpected(Error::NotMapped);
    }

    const PageSize p_size = validate_vma->flags.get_page_size();
    const std::size_t bpp = get_page_bytes(p_size);
    const auto chunk_end = VirtualAddress{std::min(validate_vma->end.value(), end_addr.value())};

    if (!validate_addr.is_aligned(bpp) || !chunk_end.is_aligned(bpp)) {
      return std::unexpected(Error::InvalidAlignment);
    }

    validate_addr = chunk_end;
    validate_vma = validate_vma->next_vma;
  }

  VMArea *curr_vma = first_vma;
  VirtualAddress curr_addr = addr;

  while (curr_addr < end_addr) {
    if (curr_addr > curr_vma->start) {
      auto res = split_vma(curr_vma, curr_addr);
      if (!res) {
        return std::unexpected(res.error());
      }

      curr_vma = *res;
    }

    const auto chunk_end = VirtualAddress{std::min(curr_vma->end.value(), end_addr.value())};
    if (chunk_end < curr_vma->end) {
      auto res = split_vma(curr_vma, chunk_end);
      if (!res) {
        return std::unexpected(res.error());
      }
    }

    // Mark this specific chunk as excluded from the swap daemon
    curr_vma->flags.set_is_locked(true);

    const AccessFlags fault_type = curr_vma->flags.get_access();
    const PageSize p_size = curr_vma->flags.get_page_size();
    const std::size_t bpp = get_page_bytes(p_size);
    const std::size_t chunk_size = chunk_end.value() - curr_addr.value();

    // Prefault the pages immediately
    for (std::size_t i = 0; i < chunk_size; i += bpp) {
      const VirtualAddress fault_page = curr_addr + i;

      if (!m_pagemap.translate(fault_page)) {
        const auto fault_res = resolve_vma_fault_locked(curr_vma, fault_page, fault_type);
        if (!fault_res) {
          return std::unexpected(fault_res.error());
        }
      }
    }

    curr_addr = chunk_end;
    curr_vma = curr_vma->next_vma;
    merge_vma(curr_vma);
  }

  return {};
}

auto AddressSpace::unlock_memory(const VirtualAddress addr, const std::size_t size_bytes) noexcept
    -> std::expected<void, Error> {
  if (!addr.is_aligned(PAGE_SIZE) || !utils::maths::is_aligned(size_bytes, PAGE_SIZE)) {
    return std::unexpected(Error::InvalidAlignment);
  }

  utils::ExclusivePreemptGuard guard(m_as_lock);

  std::uint64_t floor_key;
  VMArea *first_vma = nullptr;
  if (!m_vma_tree.get_floor(addr.value(), floor_key, first_vma) || addr < first_vma->start) {
    return std::unexpected(Error::NotMapped);
  }

  const VirtualAddress end_addr = addr + size_bytes;

  VMArea *validate_vma = first_vma;
  VirtualAddress validate_addr = addr;
  while (validate_addr < end_addr) {
    if (!validate_vma || validate_vma->start > validate_addr) {
      return std::unexpected(Error::NotMapped);
    }

    const PageSize p_size = validate_vma->flags.get_page_size();
    const std::size_t bpp = get_page_bytes(p_size);
    const auto chunk_end = VirtualAddress{std::min(validate_vma->end.value(), end_addr.value())};

    if (!validate_addr.is_aligned(bpp) || !chunk_end.is_aligned(bpp)) {
      return std::unexpected(Error::InvalidAlignment);
    }

    validate_addr = chunk_end;
    validate_vma = validate_vma->next_vma;
  }

  VMArea *curr_vma = first_vma;
  VirtualAddress curr_addr = addr;

  while (curr_addr < end_addr) {
    if (curr_addr > curr_vma->start) {
      auto res = split_vma(curr_vma, curr_addr);
      if (!res) {
        return std::unexpected(res.error());
      }

      curr_vma = *res;
    }

    const VirtualAddress chunk_end = VirtualAddress{std::min(curr_vma->end.value(), end_addr.value())};

    if (chunk_end < curr_vma->end) {
      auto res = split_vma(curr_vma, chunk_end);
      if (!res) {
        return std::unexpected(res.error());
      }
    }

    curr_vma->flags.set_is_locked(false);

    curr_addr = chunk_end;
    curr_vma = curr_vma->next_vma;
    merge_vma(curr_vma);
  }

  return {};
}

auto AddressSpace::evict_page(VirtualAddress addr) noexcept -> std::expected<void, Error> {
  utils::ExclusivePreemptGuard guard(m_as_lock);

  std::uint64_t floor_key;
  VMArea *vma = nullptr;
  if (!m_vma_tree.get_floor(addr.value(), floor_key, vma) || addr < vma->start || addr >= vma->end) {
    return std::unexpected(Error::NotMapped);
  }

  if (vma->flags.get_is_wired() || vma->flags.get_is_locked()) {
    return std::unexpected(Error::SecurityViolation);
  }

  const PageSize page_size = vma->flags.get_page_size();
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const VirtualAddress aligned_addr = addr.align_down(bytes_per_page);

  if (auto res = m_pagemap.unmap_range(aligned_addr, bytes_per_page); !res) {
    return std::unexpected(res.error());
  }

  const bool is_huge = (page_size != PageSize::Size4K);
  tlb::ShootdownCoordinator::broadcast(&m_pagemap, aligned_addr, bytes_per_page, is_huge);

  return {};
}
} // namespace kernel::memory::vmm