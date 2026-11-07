/// @file tlb.hpp
/// @brief PCID allocation and TLB invalidation.

#pragma once

#include <limits>

#include "hal/hal.hpp"
#include "pagemap.hpp"

namespace kernel::memory::vmm::tlb {
/// @brief Size to pass to ShootdownCoordinator::broadcast() to drop every translation of a map.
inline constexpr std::size_t FLUSH_ALL = std::numeric_limits<std::size_t>::max();

/// @brief Per-CPU assignment of hardware PCIDs to page maps.
///
/// With PCIDs the TLB keeps entries of several address spaces at once, each tagged with a
/// 12-bit ID, so switching CR3 does not have to throw everything away. PCIDs are a per-CPU
/// resource here: each CPU hands them out from 1 upward (0 is left for code that loads CR3
/// directly) and records the assignment in an open-addressed table keyed by
/// PageMap::generation(). When all 4095 are used up the CPU starts a new *epoch*: it flushes the
/// whole TLB and treats every existing assignment as stale.
///
/// The table never stores PageMap pointers, so a map that dies while it still has slots here
/// leaves nothing dangling, and a new map allocated at the same address is never mistaken for it.
class PcidManager {
  struct Slot {
    std::uint64_t key;    ///< PageMap generation, or DEAD_KEY.
    std::uint32_t epoch;  ///< Slot is live only if this matches the current epoch.
    std::uint16_t pcid;
  };

  static constexpr std::size_t TABLE_SIZE = 8192;  // never more than half full
  static constexpr std::size_t TABLE_MASK = TABLE_SIZE - 1;
  static constexpr std::uint16_t MAX_PCID = 4095;
  // Generations start at 1, so a dead slot still terminates no probe chain but matches nothing.
  static constexpr std::uint64_t DEAD_KEY = 0;

  std::array<Slot, TABLE_SIZE> m_table{};
  std::uint32_t m_curr_epoch{1};
  std::uint16_t m_next_pcid{1};
  PageMap* m_active{nullptr};  ///< What load() put in CR3 last.

  void rollover_epoch() noexcept;

  [[nodiscard]] static constexpr std::size_t hash(const std::uint64_t key) noexcept {
    constexpr std::uint64_t GOLDEN_RATIO = 11400714819323198485ul;
    return (key * GOLDEN_RATIO) >> (64 - std::countr_zero(TABLE_SIZE));
  }

 public:
  PcidManager() noexcept = default;
  PcidManager(const PcidManager&) = delete;
  PcidManager& operator=(const PcidManager&) = delete;

  /// @brief Switches this CPU to @p pagemap, reusing its PCID if it still has one.
  void load(PageMap* pagemap) noexcept;

  /// @brief The map this CPU has loaded.
  ///
  /// Kernel threads run on whatever map was there before them, as they only use the kernel half,
  /// which is the same in all of them. So this is the map of the last user thread to run here, or
  /// the kernel's. A switch to a user thread compares against it to skip a CR3 write it does not
  /// need, and a map that is about to be freed has to get every CPU off it first (see
  /// ShootdownCoordinator::retire()).
  [[nodiscard]] PageMap* active() const noexcept { return m_active; }

  /// @return The PCID @p pagemap has on this CPU in the current epoch, or 0 if none.
  [[nodiscard]] std::uint16_t get_active_pcid(const PageMap* pagemap) const noexcept;

  /// @brief Forgets this CPU's PCID for @p pagemap; its next load() gets a fresh, flushed one.
  ///
  /// This is the fallback for invalidating a PCID that is not loaded when the CPU cannot target
  /// it directly (no INVPCID): retiring the ID is as good as flushing it.
  void invalidate_active_pcid(const PageMap* pagemap) noexcept;
};

/// @brief A cross-CPU invalidation request, as passed to every target.
struct ShootdownRequest {
  const PageMap* pmap;
  VirtualAddress base_virt;
  std::size_t size_bytes;
  PageSize stride;
};

/// @brief Keeps every CPU's TLB coherent with page table changes.
///
/// A change is first invalidated locally, then on every other CPU that may cache it: all online
/// CPUs for kernel addresses (the kernel half is shared by every address space), only those in
/// the map's tracker for user addresses. Remote CPUs are reached either with AMD's INVLPGB or
/// through hw::ipi::call_many(), which returns only once every target has flushed, so callers
/// may free the old frames as soon as broadcast() returns.
///
/// Shootdowns from different CPUs run in parallel, and a CPU waiting for its targets keeps
/// answering shootdowns aimed at itself, so two CPUs shooting at each other cannot deadlock. A
/// target does have to answer, though: never wait with interrupts disabled for a lock that a
/// page table updater may hold.
class ShootdownCoordinator {
 public:
  ShootdownCoordinator() noexcept = default;

  /// @brief Drops this CPU's stale TLB entries as it comes online.
  ///
  /// Called by the SMP code once the CPU shows up in the online set. Shootdowns issued before
  /// that skipped it, which is why everything has to go.
  static void cpu_online() noexcept;

  /// @brief Invalidates [@p virt, @p virt + @p size_bytes) of @p pmap on every CPU that may
  ///        hold translations for it, and waits until they all have.
  /// @param stride Smallest page size mapped anywhere in the range. One invalidation per stride
  ///               is issued, so passing a huge size for a range with 4 KiB pages misses
  ///               entries; Size4K is always correct. Large ranges fall back to a full flush.
  static void broadcast(PageMap* pmap, VirtualAddress virt, std::size_t size_bytes,
                        PageSize stride = PageSize::Size4K) noexcept;

  /// @brief Invalidates the range on this CPU only.
  static void local_invalidate(const PageMap* pmap, VirtualAddress virt, std::size_t size,
                               PageSize stride) noexcept;

  /// @brief Moves every CPU that has @p pmap loaded over to the kernel's map, and waits until they
  ///        all have, so that the map can be freed.
  ///
  /// Nothing that runs on the map may be left by then: no thread of it, and no code that loads it
  /// by hand. What is left is CPUs that went on to a kernel thread without loading anything.
  /// @pre The calling thread can wait for other CPUs.
  static void retire(PageMap* pmap) noexcept;

  /// @brief Issues AMD INVLPGB for the range, splitting it to fit the CPU's count limit.
  static void queue_invlpgb(VirtualAddress virt, std::size_t size_bytes, std::uint16_t target_pcid,
                            PageSize stride) noexcept;
};
}  // namespace kernel::memory::vmm::tlb
