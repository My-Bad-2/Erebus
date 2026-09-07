#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
std::expected<VMArea *, Error> AddressSpace::expand_stack_locked(const VirtualAddress fault_addr) noexcept {
  std::uint64_t floor_key;
  VMArea *floor_vma = nullptr;

  const bool found = m_vma_tree.get_floor(fault_addr.value(), floor_key, floor_vma);
  if (found && floor_vma->contains(fault_addr)) {
    return floor_vma;
  }

  VMArea *next_vma = found ? floor_vma->next_vma : m_vma_list_head;
  if (!next_vma || !has_flag(next_vma->flags.get<"access", AccessFlags>(), AccessFlags::Stack)) {
    return std::unexpected(Error::NotMapped);
  }

  const PageSize page_size = next_vma->flags.get<"page_size", PageSize>();
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const VirtualAddress new_start = fault_addr.align_down(bytes_per_page);

  constexpr std::size_t MAX_STACK_SIZE = 4 * PAGE_SIZE_2MB; // 8MB limit
  if ((next_vma->end.value() - new_start.value()) > MAX_STACK_SIZE) {
    return std::unexpected(Error::StackOverflow);
  }

  if (found && new_start < floor_vma->end) {
    return std::unexpected(Error::StackOverflow);
  }

  m_vma_tree.erase(next_vma->start.value());

  const std::size_t growth_bytes = next_vma->start - new_start;
  const std::size_t growth_pages = growth_bytes / bytes_per_page;

  next_vma->start = new_start;
  next_vma->object_offset -= growth_pages;
  m_vma_tree.insert(new_start.value(), next_vma);
  return next_vma;
}

std::expected<void, Error> AddressSpace::resolve_vma_fault_locked(VMArea *vma, VirtualAddress fault_addr,
                                                                  AccessFlags fault_type) noexcept {
  const AccessFlags vma_access = static_cast<AccessFlags>(vma->flags.get<"access">());
  const PageSize page_size = static_cast<PageSize>(vma->flags.get<"page_size">());
  const CacheMode cache = static_cast<CacheMode>(vma->flags.get<"cache">());
  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const std::uint8_t pkey = vma->flags.get<"pkey">();

  const std::size_t page_idx = vma->object_offset + ((fault_addr.value() - vma->start.value()) / bytes_per_page);

  if (has_flag(fault_type, AccessFlags::User) && !has_flag(vma_access, AccessFlags::User)) {
    return std::unexpected(Error::SecurityViolation);
  }

  if (has_flag(fault_type, AccessFlags::Execute) && !has_flag(vma_access, AccessFlags::Execute)) {
    return std::unexpected(Error::SecurityViolation);
  }

  if (has_flag(fault_type, AccessFlags::Write)) {
    if (!has_flag(vma_access, AccessFlags::Write)) {
      return std::unexpected(Error::SecurityViolation);
    }

    if (has_flag(vma->flags.get<"access", AccessFlags>(), AccessFlags::CopyOnWrite)) {
      auto cow_phys = vma->vm_object->resolve_cow_fault(page_idx, page_size);
      if (!cow_phys) {
        return std::unexpected(cow_phys.error());
      }

      [[maybe_unused]] auto _ =
          m_pagemap.map(fault_addr.align_down(bytes_per_page), *cow_phys, vma_access, cache, pkey, page_size);
      vma->fault_count++;
      return {};
    }
  }

  if (!vma->vm_object) {
    return std::unexpected(Error::InvalidAlignment);
  }

  auto phys_res = vma->vm_object->get_page(page_idx, page_size);
  if (!phys_res) {
    return std::unexpected(phys_res.error());
  }

  AccessFlags hw_flags = vma_access;
  if (has_flag(vma->flags.get<"access", AccessFlags>(), AccessFlags::CopyOnWrite)) {
    hw_flags &= ~AccessFlags::Write;
  }

  [[maybe_unused]] auto _ =
      m_pagemap.map(fault_addr.align_down(bytes_per_page), *phys_res, hw_flags, cache, pkey, page_size);
  vma->fault_count++;

  return {};
}

std::expected<void, Error> AddressSpace::handle_page_fault(const VirtualAddress fault_addr,
                                                           const AccessFlags fault_type) noexcept {
  {
    utils::SharedPreemptGuard shared_guard(m_as_lock);
    VMArea *target_vma = nullptr;

    std::uint64_t floor_key;
    VMArea *floor_vma = nullptr;
    const bool found = m_vma_tree.get_floor(fault_addr.value(), floor_key, target_vma);

    if (found && floor_vma->contains(fault_addr)) {
      target_vma = floor_vma;
    } else {
      VMArea *next_vma = found ? floor_vma->next_vma : m_vma_list_head;
      if (!next_vma || !has_flag(next_vma->flags.get<"access", AccessFlags>(), AccessFlags::Stack)) {
        return std::unexpected(Error::NotMapped);
      }

      // We found a stack vma that could grow. Fallback to the slow path.
    }

    if (target_vma) {
      return resolve_vma_fault_locked(target_vma, fault_addr, fault_type);
    }
  }

  utils::ExclusivePreemptGuard exclusive_guard(m_as_lock);

  auto expand_res = expand_stack_locked(fault_addr);
  if (!expand_res) {
    return std::unexpected(expand_res.error());
  }

  return resolve_vma_fault_locked(*expand_res, fault_addr, fault_type);
}
} // namespace kernel::memory::vmm