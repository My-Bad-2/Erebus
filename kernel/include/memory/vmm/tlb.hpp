#pragma once

#include "memory/vmm/pagemap.hpp"

#include "hal/hal.hpp"
#include "hal/patcher.hpp"

namespace kernel::memory::vmm::tlb {
struct HashSlot {
  PageMap *pmap;
  std::uint16_t pcid;
  std::uint32_t epoch;
};

class PcidManager {
  static constexpr std::size_t TABLE_SIZE = 8192;
  static constexpr std::size_t TABLE_MASK = TABLE_SIZE - 1;
  static constexpr std::uint16_t MAX_PCID = 4095;

  std::array<HashSlot, TABLE_SIZE> m_table{};
  std::uint32_t m_curr_epoch{1};
  std::uint16_t m_next_pcid{1}; // PCID 0 is reserved

  void rollover_epoch() noexcept;

  [[nodiscard]] constexpr std::size_t hash(const PageMap *pmap) const noexcept {
    const std::uint64_t ptr = reinterpret_cast<std::uint64_t>(pmap);
    constexpr std::uint64_t GOLDEN_RATIO = 11400714819323198485ull;
    return (ptr * GOLDEN_RATIO) >> (64 - 13);
  }

public:
  PcidManager() noexcept = default;
  PcidManager(const PcidManager &) = delete;
  PcidManager &operator=(const PcidManager &) = delete;

  void load(PageMap *pagemap) noexcept;
  [[nodiscard]] std::uint16_t get_active_pcid(const PageMap *pagemap) const noexcept;

  void invalidate_active_pcid(const PageMap *pagemap) noexcept;
};

struct alignas(std::hardware_destructive_interference_size) ShootdownRequest {
  const PageMap *pmap;
  VirtualAddress base_virt;
  std::size_t size_bytes;
  std::atomic<std::uint32_t> pending_count;
};

class ShootdownCoordinator {
  static void local_invalidate(const PageMap *pmap, VirtualAddress virt, std::size_t size, bool is_huge) noexcept;
  static void queue_invlpgb(VirtualAddress virt, std::size_t size_bytes, std::uint16_t target_pcid,
                            bool is_huge) noexcept;

public:
  ShootdownCoordinator() noexcept = default;
  static void broadcast(PageMap *pmap, VirtualAddress virt, std::size_t size_bytes, bool is_huge = false) noexcept;
  static void handle_ipi(ShootdownRequest *req) noexcept;
};

void initialize() noexcept;
} // namespace kernel::memory::vmm::tlb