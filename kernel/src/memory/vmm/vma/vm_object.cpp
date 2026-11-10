#include "memory/vmm/vma/vm_object.hpp"

#include <string.h>

#include "memory/pmm.hpp"
#include "memory/vmm/vma/vma.hpp"

namespace kernel::memory::vmm {
VMObject::VMObject(const VMAType type, const PageSize page_size,
                   const std::size_t max_pages) noexcept
    : m_type(type), m_page_size(page_size), m_max_pages(max_pages) {}

VMObject::~VMObject() noexcept {
  const std::uint8_t order = page_order();
  m_pages.for_each(
      [order](std::uint64_t, const PhysToken* token) { pmm::free_pages(unpack(token), order); });

  if (m_shadow_parent) {
    m_shadow_parent->drop_ref();
  }
}

void VMObject::initialize_cache() noexcept {
  const auto res = heap::KmemCache::create(sizeof(VMObject), alignof(VMObject));
  if (!res) {
    utils::logger::fatal("Unable to allocate the VMObject cache!\n");
  }

  s_cache = *res;
}

VMObject* VMObject::create(const VMAType type, const PageSize page_size,
                           const std::size_t max_pages) noexcept {
  const auto mem = s_cache->alloc();
  if (!mem) [[unlikely]] {
    return nullptr;
  }

  return new (*mem) VMObject(type, page_size, max_pages);
}

void VMObject::drop_ref() noexcept {
  if (m_refs.drop_ref()) {
    this->~VMObject();
    s_cache->free(this);
  }
}

void VMObject::expand_max_pages(const std::size_t new_max_pages) noexcept {
  std::size_t current = m_max_pages.load(std::memory_order_relaxed);
  while (current < new_max_pages) {
    if (m_max_pages.compare_exchange_weak(current, new_max_pages, std::memory_order_release,
                                          std::memory_order_relaxed)) {
      break;
    }
  }
}

std::uint8_t VMObject::page_order() const noexcept { return get_page_order(m_page_size); }

std::optional<PhysicalAddress> VMObject::lookup_local(const std::size_t page_offset) noexcept {
  if (const PhysToken* token = m_pages.lookup(page_offset)) {
    return unpack(token);
  }

  return std::nullopt;
}

std::optional<PhysicalAddress> VMObject::lookup_inherited(std::size_t page_offset) const noexcept {
  for (VMObject* obj = m_shadow_parent; obj; obj = obj->m_shadow_parent) {
    if (const auto page = obj->lookup_local(page_offset)) {
      return page;
    }
  }

  return std::nullopt;
}

std::expected<PhysicalAddress, Error> VMObject::install(const std::size_t page_offset,
                                                        const PhysicalAddress page) noexcept {
  if (m_pages.insert(page_offset, pack(page))) [[likely]] {
    return page;
  }

  pmm::free_pages(page, page_order());

  // Either a fault on another CPU installed this offset first, and its page wins, or the map
  // itself could not grow.
  if (const auto existing = lookup_local(page_offset)) {
    return *existing;
  }

  return std::unexpected(Error::OutOfMemory);
}

VMObject* VMObject::create_shadow_clone() noexcept {
  VMObject* clone = create(m_type, m_page_size, m_max_pages.load(std::memory_order_relaxed));
  if (!clone) [[unlikely]] {
    return nullptr;
  }

  add_ref();
  clone->m_shadow_parent = this;
  return clone;
}

std::expected<PhysicalAddress, Error> VMObject::get_page(const std::size_t page_offset) noexcept {
  if (page_offset >= m_max_pages.load(std::memory_order_acquire)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  if (const auto page = lookup_local(page_offset)) [[likely]] {
    return *page;
  }

  if (const auto page = lookup_inherited(page_offset)) {
    return *page;
  }

  // Memory nobody wrote yet is zeroes, private or shared. Files and devices have pages of their
  // own, which nothing fills in yet.
  if (m_type != VMAType::Anonymous && m_type != VMAType::SharedIPC) {
    return std::unexpected(Error::NotImplemented);
  }

  const auto phys_res = pmm::alloc_pages_zeroed(pmm::PageMobility::Movable, page_order());
  if (!phys_res) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  return install(page_offset, *phys_res);
}

std::expected<PhysicalAddress, Error> VMObject::resolve_cow_fault(
    const std::size_t page_offset) noexcept {
  if (page_offset >= m_max_pages.load(std::memory_order_acquire)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  // Already copied (or first touched by a read in this very object): nothing is shared.
  if (const auto page = lookup_local(page_offset)) {
    return *page;
  }

  const auto new_phys = pmm::alloc_pages(pmm::PageMobility::Movable, page_order());
  if (!new_phys) [[unlikely]] {
    return std::unexpected(Error::OutOfMemory);
  }

  void* dest = DirectMap::phys_to_virt(*new_phys).as<void>();
  const std::size_t bytes = get_page_bytes(m_page_size);

  if (const auto source = lookup_inherited(page_offset)) {
    klib::memcpy(dest, DirectMap::phys_to_virt(*source).as<void>(), bytes);
  } else {
    klib::memset(dest, 0, bytes);
  }

  return install(page_offset, *new_phys);
}
}  // namespace kernel::memory::vmm
