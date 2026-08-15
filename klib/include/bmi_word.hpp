#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <immintrin.h>

namespace klib {
class BmiWord {
  alignas(8) std::atomic<std::uint64_t> m_mask{~0ul};

  [[nodiscard]] static constexpr std::uint64_t
  clear_lowest_bit(const std::uint64_t val) noexcept {
#if defined(__BMI__)
    return _blsr_u64(val);
#else
    return val & (val - 1);
#endif
  }

  [[nodiscard]] static constexpr std::uint64_t
  generate_mask(const std::uint8_t idx, const std::uint8_t count) noexcept {
#if defined(__BMI2__)
    // _bzhi_u64 copies the source and zeroes all bits above index `count`.
    const std::uint64_t base_mask = _bzhi_u64(~0ul, count);
    return base_mask << idx;
#else
    return (count == 64) ? ~0ul : ((1ul << count) - 1) << idx;
#endif
  }

public:
  constexpr BmiWord() noexcept = default;

  constexpr explicit BmiWord(const std::uint64_t mask) noexcept
      : m_mask{mask} {}

  BmiWord(const BmiWord &) = delete;
  BmiWord &operator=(const BmiWord &) = delete;

  void release_bit(const std::uint8_t idx) noexcept {
    [[assume(idx < 64)]];
    m_mask.fetch_or(1ul << idx, std::memory_order_release);
  }

  void release_contiguous(const std::uint8_t idx,
                          const std::uint8_t count) noexcept {
    [[assume(idx < 64 && count > 0 && idx + count <= 64)]];
    const std::uint64_t free_mask = generate_mask(idx, count);
    m_mask.fetch_or(free_mask, std::memory_order_release);
  }

  [[nodiscard]] bool acquire_specific(const std::uint8_t idx) noexcept {
    [[assume(idx < 64)]];
    const std::uint64_t target = 1ul << idx;

    const std::uint64_t prev =
        m_mask.fetch_and(~target, std::memory_order_acq_rel);

    return (prev & target) != 0;
  }

  [[nodiscard]] std::uint8_t acquire_first_free() noexcept {
    std::uint64_t curr = m_mask.load(std::memory_order_acquire);

    while (curr != 0) {
      const std::uint8_t idx = std::countr_zero(curr);
      const std::uint64_t next = clear_lowest_bit(curr);

      if (m_mask.compare_exchange_weak(curr, next, std::memory_order_acq_rel))
          [[likely]] {
        return idx;
      }
    }

    return 64;
  }

  [[nodiscard]] std::uint8_t
  acquire_contiguous(const std::uint8_t count) noexcept {
    [[assume(count > 0 && count <= 64)]];
    std::uint64_t curr = m_mask.load(std::memory_order_acquire);

    while (true) {
      std::uint64_t contiguous = curr;
      std::uint8_t needed = count - 1;
      std::uint8_t shift = 1;

      while (needed > 0 && contiguous != 0) {
        contiguous &= (contiguous >> shift);
        needed -= shift;
        shift = std::min<std::uint8_t>(shift << 1, needed);
      }

      if (contiguous == 0) [[unlikely]] {
        return 64;
      }

      const std::uint8_t idx = std::countr_zero(contiguous);
      const std::uint64_t claim_mask = generate_mask(idx, count);
      const std::uint64_t next = curr & ~claim_mask;

      if (m_mask.compare_exchange_weak(curr, next, std::memory_order_acq_rel)) {
        return idx;
      }
    }
  }

  [[nodiscard]] std::uint64_t snapshot() const noexcept {
    return m_mask.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint8_t free_count() const noexcept {
    return static_cast<std::uint8_t>(std::popcount(snapshot()));
  }

  [[nodiscard]] bool is_full() const noexcept { return snapshot() == 0; }

  [[nodiscard]] bool is_empty() const noexcept { return snapshot() == ~0ul; }

  uint64_t reset(const uint64_t new_mask = ~0ul) noexcept {
    return m_mask.exchange(new_mask, std::memory_order_acq_rel);
  }
};
} // namespace klib
