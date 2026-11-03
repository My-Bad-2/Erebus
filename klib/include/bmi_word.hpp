/// @file bmi_word.hpp
/// @brief A lock-free allocator of up to 64 slots, one bit per slot.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

namespace klib {
/// @brief 64 slots, each free (bit set) or taken (bit clear), claimed and released atomically.
///
/// Built with BMI enabled, the bit tricks compile to `blsr`, `tzcnt` and `bzhi`, hence the name.
/// Every operation that finds no room returns 64.
class BmiWord {
  alignas(8) std::atomic<std::uint64_t> m_mask{~std::uint64_t{0}};

  // `count` ones starting at bit `idx`; `idx + count` must not exceed 64.
  [[nodiscard]] static constexpr std::uint64_t run_mask(const std::uint32_t idx,
                                                        const std::uint32_t count) noexcept {
    return (count >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1) << idx;
  }

 public:
  /// @brief All 64 slots free.
  constexpr BmiWord() noexcept = default;

  /// @brief Slots free where `mask` has a one.
  constexpr explicit BmiWord(const std::uint64_t mask) noexcept : m_mask{mask} {}

  BmiWord(const BmiWord&) = delete;
  BmiWord& operator=(const BmiWord&) = delete;

  /// @brief Frees slot `idx`.
  void release_bit(const std::uint32_t idx) noexcept {
    [[assume(idx < 64)]];
    m_mask.fetch_or(std::uint64_t{1} << idx, std::memory_order_release);
  }

  /// @brief Frees `count` slots starting at `idx`.
  void release_contiguous(const std::uint32_t idx, const std::uint32_t count) noexcept {
    [[assume(idx < 64 && count > 0 && idx + count <= 64)]];
    m_mask.fetch_or(run_mask(idx, count), std::memory_order_release);
  }

  /// @brief Takes slot `idx`. False if it was taken already.
  [[nodiscard]] bool acquire_specific(const std::uint32_t idx) noexcept {
    [[assume(idx < 64)]];
    const std::uint64_t target = std::uint64_t{1} << idx;
    return (m_mask.fetch_and(~target, std::memory_order_acq_rel) & target) != 0;
  }

  /// @brief Takes the lowest free slot and returns its index, or 64 if all are taken.
  [[nodiscard]] std::uint32_t acquire_first_free() noexcept {
    std::uint64_t curr = m_mask.load(std::memory_order_relaxed);

    while (curr != 0) {
      if (m_mask.compare_exchange_weak(curr, curr & (curr - 1), std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) [[likely]] {
        return static_cast<std::uint32_t>(std::countr_zero(curr));
      }
    }

    return 64;
  }

  /// @brief Takes the lowest run of `count` free slots and returns where it starts, or 64 if
  ///        there is none.
  [[nodiscard]] std::uint32_t acquire_contiguous(const std::uint32_t count) noexcept {
    [[assume(count > 0 && count <= 64)]];
    std::uint64_t curr = m_mask.load(std::memory_order_relaxed);

    while (true) {
      // Bit i of `starts` ends up set when bits i to i + count - 1 of `curr` all are. Each step
      // doubles the length of the runs it tracks, so this takes log2(count) steps.
      std::uint64_t starts = curr;
      for (std::uint32_t covered = 1; covered < count && starts != 0;) {
        const std::uint32_t step = std::min(covered, count - covered);
        starts &= starts >> step;
        covered += step;
      }

      if (starts == 0) [[unlikely]] {
        return 64;
      }

      const auto idx = static_cast<std::uint32_t>(std::countr_zero(starts));
      if (m_mask.compare_exchange_weak(curr, curr & ~run_mask(idx, count),
                                       std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return idx;
      }
    }
  }

  /// @brief The free slots right now, as a mask.
  [[nodiscard]] std::uint64_t snapshot() const noexcept {
    return m_mask.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint32_t free_count() const noexcept {
    return static_cast<std::uint32_t>(std::popcount(snapshot()));
  }

  /// @brief Whether every slot is taken.
  [[nodiscard]] bool is_full() const noexcept { return snapshot() == 0; }

  /// @brief Whether every slot is free.
  [[nodiscard]] bool is_empty() const noexcept { return snapshot() == ~std::uint64_t{0}; }

  /// @brief Replaces the whole mask and returns the old one.
  std::uint64_t reset(const std::uint64_t new_mask = ~std::uint64_t{0}) noexcept {
    return m_mask.exchange(new_mask, std::memory_order_acq_rel);
  }
};
}  // namespace klib
