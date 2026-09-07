#include "memory/pmm.hpp"
#include "memory/vmm/vma/vm_object.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
VMObject *VMObject::create_shadow_clone() noexcept {
  auto *clone = new VMObject(this->m_type, this->m_max_pages.load(std::memory_order_relaxed));
  if (!clone) {
    return nullptr;
  }

  this->add_ref();
  clone->m_shadow_parent = this;
  return clone;
}

std::expected<PhysicalAddress, Error> VMObject::get_page(const std::size_t page_offset,
                                                         const PageSize page_size) noexcept {
  if (page_offset >= m_max_pages.load(std::memory_order_acquire)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  // Is this locally allocated?
  if (const PhysToken *token = m_pages.lookup(page_offset)) [[likely]] {
    return unpack(token);
  }

  // Do we have a prent with this memory?
  if (m_shadow_parent) {
    auto parent_phys = m_shadow_parent->get_page(page_offset, page_size);
    if (parent_phys) {
      return *parent_phys;
    }
  }

  // First time access
  if (m_type == VMAType::Anonymous) {
    const std::uint8_t order = get_page_order(page_size);

    const auto phys_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, order);
    if (!phys_res) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    if (m_pages.insert(page_offset, pack(*phys_res))) {
      return *phys_res;
    }

    // We lost the race condition
    pmm::free_pages(*phys_res, order);
    return unpack(m_pages.lookup(page_offset));
  }

  return std::unexpected(Error::NotImplemented);
}

std::expected<PhysicalAddress, Error> VMObject::resolve_cow_fault(const std::size_t page_offset,
                                                                  const PageSize size) noexcept {
  const std::uint8_t order = get_page_order(size);
  const std::size_t bytes_to_copy = get_page_bytes(size);

  const auto new_phys = pmm::alloc_pages(pmm::PageMobility::Movable, order);
  if (!new_phys) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  PhysicalAddress new_phys_addr{*new_phys};
  const VirtualAddress new_virt = DirectMap::phys_to_virt(new_phys_addr);

  if (m_shadow_parent) {
    if (auto parent_phys = m_shadow_parent->get_page(page_offset, size)) {
      const VirtualAddress parent_virt = DirectMap::phys_to_virt(*parent_phys);
      klib::memcpy(parent_virt.as<void>(), new_virt.as<void>(), bytes_to_copy);
    } else {
      klib::memset(new_virt.as<void>(), 0, bytes_to_copy);
    }
  } else {
    klib::memset(new_virt.as<void>(), 0, bytes_to_copy);
  }

  if (m_pages.insert(page_offset, pack(new_phys_addr))) {
    return new_phys_addr;
  }

  pmm::free_pages(new_phys_addr, order);
  return unpack(m_pages.lookup(page_offset));
}
} // namespace kernel::memory::vmm