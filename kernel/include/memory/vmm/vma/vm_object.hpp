/// @file vm_object.hpp
/// @brief VMObject, the backing store of one or more VMAs.

#pragma once

#include <expected>

#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/pagemap/flags.hpp"
#include "ref_counter.hpp"
#include "utils/hashmap.hpp"

namespace kernel::memory::vmm {
enum class VMAType : std::uint8_t;

/// @brief A sparse array of physical pages, indexed by page offset.
///
/// Anonymous objects allocate a zeroed page the first time an offset is touched. Copy-on-write
/// is built from shadow objects: forking a private mapping gives both sides a fresh, empty
/// shadow whose parent is the original object. Reads fall through the chain to the first object
/// that has the page; the first write copies it into the writer's own shadow. The original
/// object is never written again once it has shadows, which is what keeps both sides isolated.
///
/// All pages of an object have the same size (the page size of the VMAs that map it). Objects
/// are reference counted; each VMA mapping one holds a reference, and so does every shadow.
class VMObject {
  RefCount m_refs;
  VMAType m_type;
  PageSize m_page_size;
  std::atomic<std::size_t> m_max_pages;

  // Pages missing here are looked up in the parent.
  VMObject* m_shadow_parent{nullptr};

  // Physical addresses are stored as fake pointers with bit 0 set, so that frame 0 cannot be
  // confused with "not present" (lookup() returns null for that). Most objects are small, so
  // a handful of shards is enough.
  struct PhysToken {};
  utils::FastHashMap<std::uint64_t, PhysToken, 4> m_pages;

  static inline heap::KmemCache* s_cache = nullptr;

  [[nodiscard]] static PhysicalAddress unpack(const PhysToken* token) noexcept {
    return PhysicalAddress{reinterpret_cast<std::uintptr_t>(token) & ~std::uintptr_t{1}};
  }

  [[nodiscard]] static PhysToken* pack(const PhysicalAddress addr) noexcept {
    return reinterpret_cast<PhysToken*>(addr.value() | 1);
  }

  VMObject(VMAType type, PageSize page_size, std::size_t max_pages) noexcept;
  ~VMObject() noexcept;

  [[nodiscard]] std::uint8_t page_order() const noexcept;
  [[nodiscard]] std::optional<PhysicalAddress> lookup_local(std::size_t page_offset) noexcept;
  [[nodiscard]] std::optional<PhysicalAddress> lookup_inherited(std::size_t page_offset) noexcept;
  [[nodiscard]] std::expected<PhysicalAddress, Error> install(std::size_t page_offset,
                                                              PhysicalAddress page) noexcept;

 public:
  VMObject(const VMObject&) = delete;
  VMObject& operator=(const VMObject&) = delete;

  /// @brief Creates the slab cache objects are allocated from. Called once by the VMM.
  static void initialize_cache() noexcept;

  /// @brief Creates an object holding one reference, owned by the caller.
  /// @return nullptr when out of memory.
  [[nodiscard]] static VMObject* create(VMAType type, PageSize page_size,
                                        std::size_t max_pages) noexcept;

  /// @pre The caller holds a reference already.
  void add_ref() noexcept { m_refs.add_ref(); }

  /// @brief Drops a reference; the last one frees every page the object owns.
  void drop_ref() noexcept;

  /// @brief Allows offsets up to @p new_max_pages. Never shrinks.
  void expand_max_pages(std::size_t new_max_pages) noexcept;

  [[nodiscard]] VMObject* shadow_parent() const noexcept { return m_shadow_parent; }
  [[nodiscard]] PageSize page_size() const noexcept { return m_page_size; }

  /// @brief Creates an empty object shadowing this one (and holding a reference to it).
  /// @return nullptr when out of memory.
  [[nodiscard]] VMObject* create_shadow_clone() noexcept;

  /// @brief The page at @p page_offset, for a read or a write to a non-COW mapping.
  ///
  /// Comes from this object, else from the first ancestor that has it, else a zeroed page is
  /// allocated *in this object*. A page found in an ancestor is shared and must be mapped
  /// read-only.
  [[nodiscard]] std::expected<PhysicalAddress, Error> get_page(std::size_t page_offset) noexcept;

  /// @brief The page at @p page_offset, made private to this object for writing.
  ///
  /// If the object already owns the page it is returned as is. Otherwise the ancestor's copy
  /// (or zeroes, if no ancestor has one) is copied into a newly allocated page.
  [[nodiscard]] std::expected<PhysicalAddress, Error> resolve_cow_fault(
      std::size_t page_offset) noexcept;
};

constexpr std::uint8_t get_page_order(const PageSize size) noexcept {
  return static_cast<std::uint8_t>((std::to_underlying(size) - 1) * 9);
}

constexpr std::size_t get_page_bytes(const PageSize size) noexcept {
  return PAGE_SIZE << get_page_order(size);
}
}  // namespace kernel::memory::vmm
