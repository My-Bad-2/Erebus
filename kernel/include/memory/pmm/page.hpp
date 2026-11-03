/// @file page.hpp
/// @brief `Page`, the per-frame descriptor stored in the vmemmap array.
///
/// There is exactly one `Page` for every 4 KiB physical frame below the highest usable address.
/// The array lives at `detail::g_vmemmap_base` and is indexed by PFN, so converting between a
/// frame and its descriptor is pointer arithmetic (see pfn_to_page() / page_to_pfn()).
///
/// A descriptor is 64 bytes: a reference count, a packed flags word, a packed topology word and a
/// 32-byte union whose meaning depends on who owns the frame:
///
///  - `buddy`: the frame is the *head* of a free block sitting in a buddy free list.
///  - `slub`:  the frame belongs to a slab owned by a `heap::KmemCache`.
///
/// Only the head frame of a block carries meaningful buddy metadata. The buddy allocator never
/// looks at the union of interior frames, which is why the slab allocator is free to stamp its
/// cache pointer into every frame of a multi-page slab.

#pragma once

#include <atomic>
#include <bit>
#include <bitfield.hpp>
#include <cstdint>

#include "hal/hal.hpp"

namespace kernel::memory::heap {
/// @brief Lock-free state of one slab, updated as a single 64-bit CAS.
///
/// Objects freed by a CPU that does not own the slab are pushed on an intrusive singly linked
/// list whose head is stored here as an object *index* (so it fits in 16 bits). `generation`
/// changes on every update, which keeps the CAS immune to ABA when the same object is popped and
/// pushed back between a load and the exchange.
struct SlabState {
  std::uint16_t remote_head_idx;  ///< First object on the remote free list, or 0xFFFF if empty.
  std::uint16_t in_use;           ///< Objects not on the remote free list (live or CPU-owned).
  std::uint16_t frozen : 1;       ///< Set while a CPU owns the slab (active or CPU-partial).
  std::uint16_t total : 15;       ///< Number of objects the slab holds.
  std::uint16_t generation;       ///< ABA tag, bumped by every successful update.
};

static_assert(sizeof(SlabState) == sizeof(std::uint64_t));

class KmemCache;
}  // namespace kernel::memory::heap

namespace kernel::memory::pmm {
/// @brief Largest buddy order: 2^18 pages = 1 GiB of contiguous memory.
inline constexpr std::uint8_t MAX_ORDER = 18;

/// @brief Lifecycle state of a frame.
///
/// The buddy allocator only relies on `Free`, which means "this frame is the head of a block that
/// is currently linked into a buddy free list". Everything else (allocated frames, interior
/// frames of a larger block, frames sitting in a per-CPU cache) is `Active`. The remaining
/// states are reserved for the reclaim path.
enum class PageState : std::uint8_t {
  Active = 0,
  Standby = 1,
  Modified = 2,
  Free = 3,
  Zeroed = 4,
  Bad = 5,
};

/// @brief Anti-fragmentation class of an allocation.
///
/// Every NUMA node keeps one buddy zone per class so that long-lived kernel allocations do not
/// pepper the regions that could otherwise be compacted into huge pages.
enum class PageMobility : std::uint8_t {
  Unmovable = 0,
  Movable = 1,
  Reclaimable = 2,
};

/// @brief Layout of `Page::flags`.
class Flags {
  std::uint32_t m_data;

 public:
  constexpr explicit Flags(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(PageState, state, 0, 3)
  BF_RW(PageMobility, mobility, 3, 2)
  BF_RW(std::uint8_t, order, 5, 5)  // Buddy order of the block this frame heads (0-18)
};

/// @brief Layout of `Page::topology`.
class Topology {
  std::uint64_t m_data;

 public:
  constexpr explicit Topology(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, numa_node, 0, 32)
  BF_RW(std::uint32_t, cpu_id, 32, 32)
};

/// @brief Descriptor of one physical frame. See the file comment for the ownership rules.
struct alignas(64) Page {
  /// Number of owners. Set to 1 by the allocator; the frame is released when it drops to 0.
  std::atomic<std::uint32_t> ref_count;
  /// Packed `Flags`.
  std::atomic<std::uint32_t> flags;
  /// Packed `Topology`.
  std::atomic<std::uint64_t> topology;

  union {
    struct {
      std::atomic<std::uint64_t> state;  ///< Packed heap::SlabState.
      heap::KmemCache* cache;            ///< Owning cache. Valid in *every* frame of a slab.
      Page* prev_partial;                ///< Node partial list linkage (head frame only).
      Page* next_partial;                ///< Node or CPU partial list linkage (head frame only).
      bool on_node_list;                 ///< Linked into a node partial list (node lock held).
    } slub;

    struct {
      Page* next;
      Page* prev;
    } buddy;
  };

  void inc_ref() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] bool dec_ref_and_test() noexcept {
    return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
  [[nodiscard]] std::uint32_t get_ref() const noexcept {
    return ref_count.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint8_t get_order() const noexcept {
    return Flags{flags.load(std::memory_order_relaxed)}.get_order();
  }

  void set_order(const std::uint8_t order) noexcept {
    update<Flags>(flags, [order](Flags& f) { f.set_order(order); });
  }

  [[nodiscard]] PageState get_state() const noexcept {
    return Flags{flags.load(std::memory_order_relaxed)}.get_state();
  }

  void set_state(const PageState new_state) noexcept {
    update<Flags>(flags, [new_state](Flags& f) { f.set_state(new_state); });
  }

  [[nodiscard]] PageMobility get_mobility() const noexcept {
    return Flags{flags.load(std::memory_order_relaxed)}.get_mobility();
  }

  void set_mobility(const PageMobility mobility) noexcept {
    update<Flags>(flags, [mobility](Flags& f) { f.set_mobility(mobility); });
  }

  [[nodiscard]] std::uint32_t get_numa_node() const noexcept {
    return Topology{topology.load(std::memory_order_relaxed)}.get_numa_node();
  }

  void set_numa_node(const std::uint32_t numa_id) noexcept {
    update<Topology>(topology, [numa_id](Topology& t) { t.set_numa_node(numa_id); });
  }

  void set_cpu_owner(const std::uint32_t cpu_id) noexcept {
    update<Topology>(topology, [cpu_id](Topology& t) { t.set_cpu_id(cpu_id); });
  }

  /// @brief Rewrites everything the buddy allocator cares about in one go.
  void setup_buddy_metadata(const PageState state, const PageMobility mobility,
                            const std::uint8_t order, const std::uint32_t node) noexcept {
    update<Flags>(flags, [&](Flags& f) {
      f.set_state(state);
      f.set_mobility(mobility);
      f.set_order(order);
    });
    set_numa_node(node);
  }

  /// @brief CAS the slab state from @p expected to @p desired.
  /// @return false if the state changed under us, in which case @p expected is refreshed.
  [[nodiscard]] bool try_update_slub_state(heap::SlabState& expected,
                                           const heap::SlabState desired) noexcept {
    auto old_val = std::bit_cast<std::uint64_t>(expected);
    const auto new_val = std::bit_cast<std::uint64_t>(desired);

    if (slub.state.compare_exchange_weak(old_val, new_val, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) [[likely]] {
      return true;
    }

    expected = std::bit_cast<heap::SlabState>(old_val);
    return false;
  }

  [[nodiscard]] heap::SlabState read_slub_state() const noexcept {
    return std::bit_cast<heap::SlabState>(slub.state.load(std::memory_order_acquire));
  }

 private:
  // The flag words are shared between the allocator (under a zone lock) and lock-free readers,
  // so every read-modify-write goes through a CAS loop.
  template <typename Schema, typename Word, typename Fn>
  static void update(std::atomic<Word>& word, Fn&& fn) noexcept {
    Word current = word.load(std::memory_order_relaxed);
    Word desired;
    do {
      Schema schema{current};
      fn(schema);
      desired = schema.raw();
    } while (!word.compare_exchange_weak(current, desired, std::memory_order_release,
                                         std::memory_order_relaxed));
  }
};

static_assert(sizeof(Page) == 64, "the vmemmap layout assumes 64-byte page descriptors");
}  // namespace kernel::memory::pmm
