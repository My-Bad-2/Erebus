#pragma once

#include "memory/pmm.hpp"
#include "ref_counter.hpp"

#include <expected>

namespace kernel::memory::vmm {
enum class VMAType : std::uint8_t;

class VMObject {
  VMAType m_type;
  RefCount m_ref_count;
  std::atomic<std::size_t> m_max_pages;

  // If this object doesn't have a page, it looks at the parent.
  VMObject *m_shadow_parent{nullptr};

  struct PhysToken {};
  utils::FastHashMap<std::uint64_t, PhysToken> m_pages;

  [[nodiscard]] static PhysicalAddress unpack(const PhysToken *token) noexcept {
    return PhysicalAddress{reinterpret_cast<std::uintptr_t>(token)};
  }

  [[nodiscard]] static PhysToken *pack(const PhysicalAddress addr) noexcept {
    return reinterpret_cast<PhysToken *>(addr.value());
  }

public:
  VMObject(const VMAType type, const std::size_t max_pages) : m_type(type), m_max_pages(max_pages) {}

  ~VMObject() {
    if (m_shadow_parent) {
      m_shadow_parent->drop_ref();
    }

    m_pages.for_each([](std::uint64_t, PhysToken *token) {
      const PhysicalAddress phys{reinterpret_cast<std::uintptr_t>(token)};
      pmm::free_pages(phys, 0);
    });
  }

  void add_ref() noexcept { m_ref_count.add_ref(); }

  void drop_ref() noexcept {
    if (m_ref_count.drop_ref()) {
      delete this;
    }
  }

  void expand_max_pages(const std::size_t new_max_pages) noexcept {
    std::size_t current = m_max_pages.load(std::memory_order_relaxed);
    while (current < new_max_pages) {
      if (m_max_pages.compare_exchange_weak(current, new_max_pages, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        break;
      }
    }
  }

  [[nodiscard]] VMObject *shadow_parent() const noexcept { return m_shadow_parent; }

  [[nodiscard]] VMObject *create_shadow_clone() noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> get_page(std::size_t page_offset, PageSize page_size) noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> resolve_cow_fault(std::size_t page_offset,
                                                                        PageSize size) noexcept;
};

constexpr std::uint8_t get_page_order(const PageSize size) noexcept {
  return static_cast<std::uint8_t>(std::to_underlying(size) * 9);
}

constexpr std::size_t get_page_bytes(const PageSize size) noexcept { return PAGE_SIZE << get_page_order(size); }
} // namespace kernel::memory::vmm