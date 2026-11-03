/// @file active_pcid.hpp
/// @brief Tracks which CPUs may hold TLB entries for a page map.

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>

#include "hal/smp/smp.hpp"
#include "utils/maths.hpp"

namespace kernel::hw {
/// @brief Bitmap of CPUs that loaded a page map, plus the PCID each of them used.
///
/// A CPU is marked when it loads the map and is never unmarked eagerly: with PCIDs a CPU keeps
/// translations around after switching away, so "may still have entries" is the only safe
/// question to ask. Shootdowns go to every marked CPU, and one that turns out to hold nothing
/// simply has nothing to flush.
class ActivePcidTracker {
  std::uint32_t m_blocks{};
  std::unique_ptr<std::atomic<std::uint64_t>[]> m_mask;
  std::unique_ptr<std::atomic<std::uint16_t>[]> m_cpu_pcids;

 public:
  ActivePcidTracker() noexcept {
    const std::size_t total_cpus = smp::possible_cpus();
    m_blocks = utils::maths::div_round_up(total_cpus, 64u);

    m_mask.reset(new std::atomic<std::uint64_t>[m_blocks] {});
    m_cpu_pcids.reset(new std::atomic<std::uint16_t>[total_cpus] {});
  }

  ~ActivePcidTracker() noexcept = default;

  ActivePcidTracker(const ActivePcidTracker&) = delete;
  ActivePcidTracker& operator=(const ActivePcidTracker&) = delete;

  void mark(const std::uint32_t cpu_id, const std::uint16_t pcid) noexcept {
    m_cpu_pcids[cpu_id].store(pcid, std::memory_order_relaxed);
    m_mask[cpu_id / 64].fetch_or(1ul << (cpu_id % 64), std::memory_order_release);
  }

  void unmark(const std::uint32_t cpu_id) noexcept {
    m_mask[cpu_id / 64].fetch_and(~(1ul << (cpu_id % 64)), std::memory_order_release);
  }

  [[nodiscard]] bool test(const std::uint32_t cpu_id) const noexcept {
    return (m_mask[cpu_id / 64].load(std::memory_order_relaxed) & (1ul << (cpu_id % 64))) != 0;
  }

  /// @brief Calls `callback(cpu_id, pcid)` for every marked CPU.
  template <typename Func>
  void for_each_active_cpu(Func&& callback) const noexcept {
    for (std::uint32_t i = 0; i < m_blocks; ++i) {
      std::uint64_t block = m_mask[i].load(std::memory_order_acquire);

      while (block != 0) {
        const std::uint32_t bit = std::countr_zero(block);
        const std::uint32_t target_cpu = (i * 64) + bit;
        const std::uint16_t target_pcid = m_cpu_pcids[target_cpu].load(std::memory_order_relaxed);

        callback(target_cpu, target_pcid);
        block &= (block - 1);
      }
    }
  }
};
}  // namespace kernel::hw
