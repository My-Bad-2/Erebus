/// @file pte.hpp
/// @brief A page table slot with atomic accessors, and builders for its contents.

#pragma once

#include <atomic>
#include <cstdint>

#include "features.hpp"
#include "flags.hpp"
#include "memory/address.hpp"
#include "memory/memory.hpp"

namespace kernel::memory::vmm {
/// @brief One 64-bit slot of a paging structure.
///
/// Page tables are walked by the MMU and modified concurrently by several CPUs, so every access
/// goes through std::atomic_ref. Updates that depend on the old value use cas(); the PageMap
/// code is written so that a failed CAS simply means "someone else got there first, look again".
struct PageTableEntry {
 private:
  std::uint64_t m_raw{0};

  [[nodiscard]] std::atomic_ref<std::uint64_t> atomic() noexcept { return std::atomic_ref{m_raw}; }
  [[nodiscard]] std::atomic_ref<const std::uint64_t> atomic() const noexcept {
    return std::atomic_ref{m_raw};
  }

 public:
  constexpr PageTableEntry() noexcept = default;
  constexpr explicit PageTableEntry(const std::uint64_t raw) noexcept : m_raw{raw} {}

  [[nodiscard]] PTEntry load(
      const std::memory_order order = std::memory_order_acquire) const noexcept {
    return PTEntry{atomic().load(order)};
  }

  void store(const PTEntry schema,
             const std::memory_order order = std::memory_order_release) noexcept {
    atomic().store(schema, order);
  }

  /// @brief Compare-and-swap. On failure @p expected receives the current value.
  [[nodiscard]] bool cas(PTEntry& expected, const PTEntry& desired,
                         const std::memory_order success = std::memory_order_acq_rel,
                         const std::memory_order failure = std::memory_order_acquire) noexcept {
    std::uint64_t expected_raw = expected;
    const bool result = atomic().compare_exchange_strong(expected_raw, desired, success, failure);

    expected = PTEntry{expected_raw};
    return result;
  }

  void clear(const std::memory_order order = std::memory_order_release) noexcept {
    atomic().store(0, order);
  }

  [[nodiscard]] bool is_present() const noexcept {
    return load(std::memory_order_relaxed).get_present() == 1;
  }
  [[nodiscard]] bool is_cow() const noexcept {
    return load(std::memory_order_relaxed).get_cow() == 1;
  }
  [[nodiscard]] bool is_swapped() const noexcept {
    return load(std::memory_order_relaxed).get_swapped() == 1;
  }
  [[nodiscard]] bool is_huge() const noexcept { return load(std::memory_order_relaxed).get_huge(); }

  /// @brief Frame address of the entry, interpreted as a leaf of @p context_size.
  /// @return std::nullopt for swapped-out entries, whose frame field is a swap slot.
  [[nodiscard]] std::optional<PhysicalAddress> extract_address(
      const PageSize context_size) const noexcept {
    const PTEntry schema = load(std::memory_order_relaxed);

    if (schema.get_swapped() == 1) {
      return std::nullopt;
    }

    switch (context_size) {
      case PageSize::Size4K:
        return PhysicalAddress{schema.get_pfn_4k() << PAGE_SHIFT_4KB};
      case PageSize::Size2M:
        return PhysicalAddress{schema.get_pfn_2m() << PAGE_SHIFT_2MB};
      case PageSize::Size1G:
        return PhysicalAddress{schema.get_pfn_1g() << PAGE_SHIFT_1GB};
    }

    std::unreachable();
  }

  /// @brief Entry pointing at a next-level table.
  ///
  /// Directories are always present and writable; permissions are decided at the leaf. The user
  /// bit is only set in the lower half, so a kernel leaf that somehow ended up with U=1 is still
  /// not reachable from ring 3.
  [[nodiscard]] static constexpr PTEntry build_directory(const PhysicalAddress table_phys,
                                                         const bool user) noexcept {
    return PTEntry{}.with_present().with_rw().with_user(user).with_pfn_4k(table_phys.value() >>
                                                                          PAGE_SHIFT_4KB);
  }

  /// @brief Leaf entry mapping @p phys with the given attributes.
  ///
  /// Copy-on-write mappings are always read-only in hardware; the write fault is what triggers
  /// the copy. A Swapped entry is built non-present.
  [[nodiscard]] static PTEntry build_schema(const PhysicalAddress phys, const AccessFlags flags,
                                            const CacheMode cache, const PageSize size,
                                            const std::uint8_t pkey = 0) noexcept {
    // Matches the PAT layout programmed by initialize_hw(): index = PAT*4 + PCD*2 + PWT.
    bool pwt = false, pcd = false, pat = false;
    switch (cache) {
      case CacheMode::WriteBack:
        break;
      case CacheMode::WriteThrough:
        pwt = true;
        break;
      case CacheMode::UncacheableMinus:
        pcd = true;
        break;
      case CacheMode::Uncacheable:
        pwt = pcd = true;
        break;
      case CacheMode::WriteCombining:
        pat = true;
        break;
      case CacheMode::WriteProtected:
        pwt = pat = true;
        break;
    }

    const bool cow = has_flag(flags, AccessFlags::CopyOnWrite);

    PTEntry entry{};

    entry.set_present(!has_flag(flags, AccessFlags::Swapped));
    entry.set_rw(has_flag(flags, AccessFlags::Write) && !cow);
    entry.set_user(has_flag(flags, AccessFlags::User));
    entry.set_global(has_flag(flags, AccessFlags::Global));
    // Without EFER.NXE bit 63 is reserved and setting it faults.
    entry.set_nx(g_paging_features.nx && !has_flag(flags, AccessFlags::Execute));
    entry.set_cow(cow);
    entry.set_shared(has_flag(flags, AccessFlags::Shared));
    entry.set_swapped(has_flag(flags, AccessFlags::Swapped));
    entry.set_stack(has_flag(flags, AccessFlags::Stack));
    entry.set_pkey(pkey);
    entry.set_pwt(pwt);
    entry.set_pcd(pcd);

    if (size == PageSize::Size4K) {
      entry.set_pat_4k(pat);
      entry.set_pfn_4k(phys.value() >> PAGE_SHIFT_4KB);
    } else if (size == PageSize::Size2M) {
      entry.set_huge(true);
      entry.set_pat_large(pat);
      entry.set_pfn_2m(phys.value() >> PAGE_SHIFT_2MB);
    } else {
      entry.set_huge(true);
      entry.set_pat_large(pat);
      entry.set_pfn_1g(phys.value() >> PAGE_SHIFT_1GB);
    }

    return entry;
  }
};
}  // namespace kernel::memory::vmm
