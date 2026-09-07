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

  [[nodiscard]] PteSchema load(const std::memory_order order = std::memory_order_acquire) const noexcept {
    return PteSchema{atomic().load(order)};
  }

  void store(const PteSchema schema, const std::memory_order order = std::memory_order_release) noexcept {
    atomic().store(schema, order);
  }

  [[nodiscard]] bool cas(PteSchema &expected, const PteSchema &desired,
                         const std::memory_order success = std::memory_order_acq_rel,
                         const std::memory_order failure = std::memory_order_acquire) noexcept {
    std::uint64_t expected_raw = expected;
    const bool result = atomic().compare_exchange_strong(expected_raw, desired, success, failure);

    expected = PteSchema{expected_raw};
    return result;
  }

  void clear(const std::memory_order order = std::memory_order_release) noexcept { atomic().store(0, order); }

  [[nodiscard]] bool is_present() const noexcept {
    return load(std::memory_order_relaxed).small().get<"present">() == 1;
  }

  [[nodiscard]] bool is_cow() const noexcept { return load(std::memory_order_relaxed).small().get<"cow">() == 1; }

  [[nodiscard]] bool is_swapped() const noexcept {
    return load(std::memory_order_relaxed).small().get<"swapped">() == 1;
  }

  [[nodiscard]] bool is_huge() const noexcept { return (load(std::memory_order_relaxed).raw & (1ul << 7)) != 0; }

  [[nodiscard]] std::optional<PhysicalAddress> extract_address(const PageSize context_size) const noexcept {
    const PteSchema schema = load(std::memory_order_relaxed);

    if (schema.small().get<"swapped">() == 1) {
      return std::nullopt;
    }

    switch (context_size) {
    case PageSize::Size4K:
      return PhysicalAddress{schema.small().get<"pfn">() << 12};
    case PageSize::Size2M:
      return PhysicalAddress{schema.huge().get<"pfn">() << 21};
    case PageSize::Size1G:
      return PhysicalAddress{schema.gig().get<"pfn">() << 30};
    }

    std::unreachable();
  }

  [[nodiscard]] static constexpr PteSchema build_schema(const PhysicalAddress phys, const AccessFlags flags,
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
      pwt = true;
      pcd = true;
      break;
    case CacheMode::WriteCombining:
      pat = true;
      break;
    case CacheMode::WriteProtected:
      pwt = true;
      pcd = true;
      pat = true;
      break;
    }

    const bool cow = has_flag(flags, AccessFlags::CopyOnWrite);

    auto apply_common = [&](auto &s) {
      s.template set_mut<"present">(has_flag(flags, AccessFlags::Swapped) ? 0 : 1);
      s.template set_mut<"rw">(has_flag(flags, AccessFlags::Write) && !cow ? 1 : 0);
      s.template set_mut<"user">(has_flag(flags, AccessFlags::User) ? 1 : 0);
      s.template set_mut<"global">(has_flag(flags, AccessFlags::Global) ? 1 : 0);
      s.template set_mut<"nx">(has_flag(flags, AccessFlags::Execute) ? 0 : 1);
      s.template set_mut<"cow">(cow ? 1 : 0);
      s.template set_mut<"shared">(has_flag(flags, AccessFlags::Shared) ? 1 : 0);
      s.template set_mut<"swapped">(has_flag(flags, AccessFlags::Swapped) ? 1 : 0);
      s.template set_mut<"pkey">(pkey);
      s.template set_mut<"pwt">(pwt ? 1 : 0);
      s.template set_mut<"pcd">(pcd ? 1 : 0);
    };

    if (size == PageSize::Size4K) {
      PteLeafSchema s{};
      apply_common(s);
      s.set_mut<"pat">(pat ? 1 : 0);
      s.set_mut<"pfn">(phys.value() >> 12);
      return PteSchema{static_cast<std::uint64_t>(s)};
    }

    if (size == PageSize::Size2M) {
      PteHugeSchema s{};
      apply_common(s);
      s.set_mut<"huge">(1);
      s.set_mut<"pat">(pat ? 1 : 0);
      s.set_mut<"pfn">(phys.value() >> 21);
      return PteSchema{static_cast<std::uint64_t>(s)};
    }

    // Size1G
    Pte1gSchema s{};
    apply_common(s);
    s.set_mut<"huge">(1);
    s.set_mut<"pat">(pat ? 1 : 0);
    s.set_mut<"pfn">(phys.value() >> 30);
    return PteSchema{static_cast<std::uint64_t>(s)};
  }
};
} // namespace kernel::memory::vmm