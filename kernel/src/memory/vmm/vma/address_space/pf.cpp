#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
namespace {
// A racing fault on another CPU may have installed the same translation first.
[[nodiscard]] std::expected<void, Error> tolerate_race(
    const std::expected<void, Error>& res) noexcept {
  if (!res && res.error() != Error::AlreadyMapped) {
    return std::unexpected(res.error());
  }

  return {};
}
}  // namespace

std::expected<VMArea*, Error> AddressSpace::expand_stack_locked(
    const VirtualAddress fault_addr) noexcept {
  // Someone may have grown it while we were waiting for the exclusive lock.
  if (VMArea* vma = find_vma(fault_addr)) {
    return vma;
  }

  std::uint64_t floor_key;
  VMArea* floor_vma = nullptr;
  const bool found = m_vma_tree.get_floor(fault_addr.value(), floor_key, floor_vma);

  VMArea* stack = found ? floor_vma->next_vma : m_vma_list_head;
  if (!stack || !has_flag(stack->flags.get_access(), AccessFlags::Stack)) {
    return std::unexpected(Error::NotMapped);
  }

  const std::size_t bytes_per_page = stack->page_bytes();
  const VirtualAddress new_start = fault_addr.align_down(bytes_per_page);
  const std::size_t growth_pages = (stack->start - new_start) / bytes_per_page;

  // The size limit and the object headroom reserved by alloc() bound the growth; the VMA below
  // must not be reached either.
  if (static_cast<std::size_t>(stack->end - new_start) > MAX_STACK_BYTES ||
      growth_pages > stack->object_offset || (found && new_start < floor_vma->end)) {
    return std::unexpected(Error::StackOverflow);
  }

  // Insert under the new key before dropping the old one; only the insert can fail.
  if (!m_vma_tree.insert(new_start.value(), stack)) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  m_vma_tree.erase(stack->start.value());
  stack->start = new_start;
  stack->object_offset -= growth_pages;
  return stack;
}

std::expected<void, Error> AddressSpace::resolve_vma_fault_locked(
    VMArea* vma, const VirtualAddress fault_addr, const AccessFlags fault_type) noexcept {
  const AccessFlags vma_access = vma->flags.get_access();
  const PageSize page_size = vma->flags.get_page_size();
  const CacheMode cache = vma->flags.get_cache();
  const std::uint8_t pkey = vma->flags.get_pkey();

  const bool is_write = has_flag(fault_type, AccessFlags::Write);

  if ((has_flag(fault_type, AccessFlags::User) && !has_flag(vma_access, AccessFlags::User)) ||
      (has_flag(fault_type, AccessFlags::Execute) && !has_flag(vma_access, AccessFlags::Execute)) ||
      (is_write && !has_flag(vma_access, AccessFlags::Write))) {
    return std::unexpected(Error::SecurityViolation);
  }

  // Wired VMAs are mapped up front, so a fault in one is a genuine error.
  VMObject* obj = vma->vm_object;
  if (!obj) {
    return std::unexpected(Error::NotMapped);
  }

  const std::size_t bytes_per_page = get_page_bytes(page_size);
  const VirtualAddress page_addr = fault_addr.align_down(bytes_per_page);
  const std::size_t page_idx = vma->object_offset + (page_addr - vma->start) / bytes_per_page;
  const bool is_cow = has_flag(vma_access, AccessFlags::CopyOnWrite);
  const auto present = m_pagemap.translate(page_addr);

  std::atomic_ref{vma->fault_count}.fetch_add(1, std::memory_order_relaxed);

  if (is_write && is_cow && (!present || !has_flag(present->flags, AccessFlags::Write))) {
    const auto private_page = obj->resolve_cow_fault(page_idx);
    if (!private_page) {
      return std::unexpected(private_page.error());
    }

    // The page now belongs to this object alone, so it can be mapped writable straight away.
    const AccessFlags flags = vma_access & ~AccessFlags::CopyOnWrite;
    if (present) {
      return m_pagemap.remap(page_addr, *private_page, flags, cache, pkey, page_size);
    }

    return tolerate_race(m_pagemap.map(page_addr, *private_page, flags, cache, pkey, page_size));
  }

  if (present) {
    // Either another CPU resolved this fault first, or our TLB held an outdated entry for a
    // translation that has since been upgraded. If the access is allowed now, drop the stale
    // entry and retry the instruction.
    if (is_write && !has_flag(present->flags, AccessFlags::Write)) [[unlikely]] {
      return std::unexpected(Error::MemoryProtectionFault);
    }

    hw::invlpg(page_addr.value());
    return {};
  }

  const auto page = obj->get_page(page_idx);
  if (!page) {
    return std::unexpected(page.error());
  }

  // In a copy-on-write VMA the page may come from a shared ancestor. vma_access still carries
  // CopyOnWrite, which makes the PTE read-only, so the first write ends up above.
  return tolerate_race(m_pagemap.map(page_addr, *page, vma_access, cache, pkey, page_size));
}

std::expected<void, Error> AddressSpace::handle_page_fault(const VirtualAddress fault_addr,
                                                           const AccessFlags fault_type) noexcept {
  {
    utils::SharedPreemptGuard shared_guard(m_as_lock);

    if (VMArea* vma = find_vma(fault_addr)) [[likely]] {
      return resolve_vma_fault_locked(vma, fault_addr, fault_type);
    }

    // Only a fault right below a stack VMA can still be satisfied, by growing the stack.
    std::uint64_t floor_key;
    VMArea* floor_vma = nullptr;
    const bool found = m_vma_tree.get_floor(fault_addr.value(), floor_key, floor_vma);
    const VMArea* next_vma = found ? floor_vma->next_vma : m_vma_list_head;

    if (!next_vma || !has_flag(next_vma->flags.get_access(), AccessFlags::Stack)) {
      return std::unexpected(Error::NotMapped);
    }
  }

  // Growing changes the VMA, which needs the lock exclusively.
  utils::ExclusivePreemptGuard exclusive_guard(m_as_lock);

  const auto stack = expand_stack_locked(fault_addr);
  if (!stack) {
    return std::unexpected(stack.error());
  }

  return resolve_vma_fault_locked(*stack, fault_addr, fault_type);
}
}  // namespace kernel::memory::vmm
