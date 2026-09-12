#pragma once

#include <atomic>
#include <cstdint>

#include "flags.hpp"
#include "memory/address.hpp"

namespace kernel::memory::vmm {
struct PageTableEntry {
private:
  std::uint64_t m_raw{0};

  [[nodiscard]] std::atomic_ref<std::uint64_t> atomic() noexcept { return std::atomic_ref{m_raw}; }
  [[nodiscard]] std::atomic_ref<const std::uint64_t> atomic() const noexcept { return std::atomic_ref{m_raw}; }

public:
  constexpr PageTableEntry() noexcept = default;
  constexpr explicit PageTableEntry(const std::uint64_t raw) noexcept : m_raw{raw} {}

  [[nodiscard]] PTEntry load(const std::memory_order order = std::memory_order_acquire) const noexcept {
    return PTEntry{atomic().load(order)};
  }

  void store(const PTEntry schema, const std::memory_order order = std::memory_order_release) noexcept {
    atomic().store(schema, order);
  }

  [[nodiscard]] bool cas(PTEntry &expected, const PTEntry &desired,
                         const std::memory_order success = std::memory_order_acq_rel,
                         const std::memory_order failure = std::memory_order_acquire) noexcept {
    std::uint64_t expected_raw = expected;
    const bool result = atomic().compare_exchange_strong(expected_raw, desired, success, failure);

    expected = PTEntry{expected_raw};
    return result;
  }

  void clear(const std::memory_order order = std::memory_order_release) noexcept { atomic().store(0, order); }

  [[nodiscard]] bool is_present() const noexcept { return load(std::memory_order_relaxed).get_present() == 1; }
  [[nodiscard]] bool is_cow() const noexcept { return load(std::memory_order_relaxed).get_cow() == 1; }
  [[nodiscard]] bool is_swapped() const noexcept { return load(std::memory_order_relaxed).get_swapped() == 1; }
  [[nodiscard]] bool is_huge() const noexcept { return load(std::memory_order_relaxed).get_huge(); }

  [[nodiscard]] std::optional<PhysicalAddress> extract_address(const PageSize context_size) const noexcept {
    const PTEntry schema = load(std::memory_order_relaxed);

    if (schema.get_swapped() == 1) {
      return std::nullopt;
    }

    switch (context_size) {
    case PageSize::Size4K:
      return PhysicalAddress{schema.get_pfn_4k() << 12};
    case PageSize::Size2M:
      return PhysicalAddress{schema.get_pfn_2m() << 21};
    case PageSize::Size1G:
      return PhysicalAddress{schema.get_pfn_1g() << 30};
    }

    std::unreachable();
  }

  [[nodiscard]] static constexpr PTEntry build_directory(const PhysicalAddress table_phys) noexcept {
    return PTEntry{}.with_present().with_rw().with_user().with_pfn_4k(table_phys.value() >> PAGE_SHIFT_4KB);
  }

  [[nodiscard]] static constexpr PTEntry build_schema(const PhysicalAddress phys, const AccessFlags flags,
                                                      const CacheMode cache, const PageSize size,
                                                      const std::uint8_t pkey = 0) noexcept {
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
    entry.set_nx(!has_flag(flags, AccessFlags::Execute));
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
} // namespace kernel::memory::vmm