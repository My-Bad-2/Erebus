#pragma once

#include "memory/vmm/pagemap/flags.hpp"
#include "utils/bplus_tree.hpp"
#include "vm_object.hpp"
#include <cstdint>

namespace kernel::memory::vmm {
enum class VMAType : std::uint8_t {
  Anonymous,
  FileBacked,
  SharedIPC,
  HardwareMMIO,
};

enum class NUMAPolicy : std::uint8_t {
  StrictLocal = 0,
  Interleave = 1,
  FirstTouch = 2,
};

using VMAFlagsSchema = klib::BitfieldSchema<klib::Field<"access", 0, 16>,    // AccessFlags
                                            klib::Field<"cache", 16, 3>,     // CacheMode
                                            klib::Field<"page_size", 19, 2>, // PageSize
                                            klib::Field<"type", 21, 4>,      // VMAType
                                            klib::Field<"numa", 25, 3>,      // NUMAPolicy
                                            klib::Bit<"is_locked", 28>,      // Excluded from swap
                                            klib::Bit<"is_wired", 29>,       // Pinned in memory
                                            klib::Field<"pkey", 30, 4>,      // Protection key
                                            klib::Reserved<34, 30>>;

struct alignas(std::hardware_destructive_interference_size) VMArea {
  VirtualAddress start;
  VirtualAddress end;

  VMObject *vm_object;
  std::size_t object_offset;
  VMAFlagsSchema flags;

  VMArea *next_vma;
  VMArea *prev_vma;

  std::uint32_t preferred_numa_node;
  std::uint32_t fault_count;

  [[nodiscard]] bool contains(const VirtualAddress addr) const noexcept {
    return addr.value() >= start.value() && addr.value() < end.value();
  }

  [[nodiscard]] std::size_t size_bytes() const noexcept { return end.value() - start.value(); }
};

class AddressSpace {
  static inline heap::KmemCache *s_vmo_cache = nullptr;
  static inline heap::KmemCache *s_vma_cache = nullptr;

  utils::BPlusTree<std::uint64_t, VMArea *, 256> m_vma_tree;

  VMArea *m_vma_list_head{nullptr};
  VMArea *m_vma_list_tail{nullptr};

  PageMap m_pagemap;
  mutable utils::RWLock m_as_lock;

  VirtualAddress m_heap_bump;
  VirtualAddress m_mmap_bump;

  [[nodiscard]] VirtualAddress find_free_gap(std::size_t size_bytes, std::size_t alignment,
                                             bool is_heap = false) noexcept;
  void link_vma(VMArea *new_vma) noexcept;
  void unlink_vma(VMArea *vma) noexcept;
  auto split_vma(VMArea *vma, VirtualAddress split_point) noexcept -> std::expected<VMArea *, Error>;

  void merge_vma(VMArea *vma) noexcept;

  [[nodiscard]] auto expand_stack_locked(VirtualAddress fault_addr) noexcept -> std::expected<VMArea *, Error>;
  [[nodiscard]] auto resolve_vma_fault_locked(VMArea *vma, VirtualAddress fault_addr, AccessFlags fault_type) noexcept
      -> std::expected<void, Error>;

public:
  explicit AddressSpace(VirtualAddress base_addr) noexcept;
  ~AddressSpace() noexcept;

  static void initialize() noexcept;

  [[nodiscard]] PageMap &pagemap() noexcept { return m_pagemap; }

  [[nodiscard]] auto alloc(std::size_t size_bytes, AccessFlags access, CacheMode cache = CacheMode::WriteBack,
                           PageSize page_size = PageSize::Size4K, VMAType type = VMAType::Anonymous,
                           std::uint8_t pkey = 0, VirtualAddress hint = VirtualAddress{}) noexcept
      -> std::expected<VirtualAddress, Error>;
  [[nodiscard]] auto free(VirtualAddress addr) noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto realloc(VirtualAddress old_addr, std::size_t new_size) noexcept
      -> std::expected<VirtualAddress, Error>;
  [[nodiscard]] auto protect(VirtualAddress addr, std::size_t size_bytes, AccessFlags new_flags) noexcept
      -> std::expected<void, Error>;

  auto map_mmio(PhysicalAddress phys_base, std::size_t size_bytes,
                AccessFlags access = AccessFlags::Read | AccessFlags::Write | AccessFlags::Mmio,
                CacheMode cache = CacheMode::Uncacheable, PageSize page_size = PageSize::Size4K, std::uint8_t pkey = 0,
                VirtualAddress hint = VirtualAddress{}) noexcept -> std::expected<VirtualAddress, Error>;

  [[nodiscard]] auto lock_memory(VirtualAddress addr, std::size_t size_bytes) noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto unlock_memory(VirtualAddress addr, std::size_t size_bytes) noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto clone_into(AddressSpace &child) noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto map_object(VMObject *existing_obj, std::size_t size_bytes, std::size_t obj_offset_pages,
                                AccessFlags access, CacheMode cache = CacheMode::WriteBack,
                                PageSize page_size = PageSize::Size4K, VMAType type = VMAType::SharedIPC,
                                std::uint8_t pkey = 0, VirtualAddress hint = VirtualAddress{}) noexcept
      -> std::expected<VirtualAddress, Error>;

  [[nodiscard]] auto create_alias(VirtualAddress addr, std::size_t size_bytes, AccessFlags alias_flags) noexcept
      -> std::expected<VirtualAddress, Error>;
  [[nodiscard]] auto destroy_alias(const VirtualAddress alias_addr) noexcept -> std::expected<void, Error> {
    return free(alias_addr);
  }

  [[nodiscard]] auto evict_page(VirtualAddress addr) noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto handle_page_fault(VirtualAddress fault_addr, AccessFlags fault_type) noexcept
      -> std::expected<void, Error>;

  template <typename Func>
    requires std::invocable<Func, const VMArea &>
  void for_each_vma(Func &&callback) const noexcept {
    utils::SharedPreemptGuard guard(m_as_lock);

    const VMArea *curr = m_vma_list_head;
    while (curr) {
      callback(*curr);
      curr = curr->next_vma;
    }
  }
};

const AddressSpace *kernel_space() noexcept;
} // namespace kernel::memory::vmm