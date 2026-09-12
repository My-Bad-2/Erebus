#pragma once

#include <atomic>
#include <cstdint>

#include "hal/hal.hpp"
#include <bitfield.hpp>

namespace kernel::memory::heap {
struct SlabState {
  std::uint16_t remote_head_idx; // Index of the first free object
  std::uint16_t in_use;          // Total objects allocated from this slab
  std::uint16_t frozen : 1;      // 1 = actively bumping on a CPU
  std::uint16_t total : 15;      // Max Capacity
  std::uint16_t generation;      // aba prevention tag
};

class KmemCache;
} // namespace kernel::memory::heap

namespace kernel::memory::pmm {
constexpr std::uint8_t MAX_ORDER = 18; // 1GB of contiguous memory

enum class PageState : std::uint8_t {
  Active = 0,
  Standby = 1,
  Modified = 2,
  Free = 3,
  Zeroed = 4,
  Bad = 5,
};

enum class PageMobility : std::uint8_t {
  Unmovable = 0,
  Movable = 1,
  Reclaimable = 2,
};

class Flags {
  std::uint32_t m_data;

public:
  constexpr explicit Flags(const std::uint32_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint32_t raw() const noexcept { return m_data; }

  BF_RW(PageState, state, 0, 3)
  BF_RW(PageMobility, mobility, 3, 2)
  BF_RW(std::uint8_t, order, 5, 5) // Buddy system order (0-18)
};

class Topology {
  std::uint64_t m_data;

public:
  constexpr explicit Topology(const std::uint64_t val = 0) : m_data(val) {}
  [[nodiscard]] std::uint64_t raw() const noexcept { return m_data; }

  BF_RW(std::uint32_t, numa_node, 0, 32)
  BF_RW(std::uint32_t, cpu_id, 32, 32)
};

struct alignas(64) Page {
  std::atomic<std::uint32_t> ref_count;
  std::atomic<std::uint32_t> flags;
  std::atomic<std::uint64_t> topology;

  union {
    struct {
      Page *next;
      std::uint64_t aba_counter;
    } freelist;

    struct {
      std::atomic<std::uint64_t> state;
      heap::KmemCache *cache;
      Page *prev_partial;
      Page *next_partial;
    } slub;

    struct {
      Page *next;
      Page *prev;
    } buddy;
  };

  void inc_ref() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] bool dec_ref_and_test() noexcept { return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1; }
  [[nodiscard]] std::uint32_t get_ref() const noexcept { return ref_count.load(std::memory_order_relaxed); }

  void set_order(const std::uint8_t order) noexcept {
    auto current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;
    do {
      Flags schema{current};
      schema.set_order(order);
      desired = schema.raw();
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  [[nodiscard]] std::uint8_t get_order() const noexcept {
    return Flags{flags.load(std::memory_order_relaxed)}.get_order();
  }

  [[nodiscard]] PageState get_state() const noexcept {
    return Flags{flags.load(std::memory_order_relaxed)}.get_state();
  }

  void set_state(const PageState new_state) noexcept {
    std::uint32_t current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;
    do {
      Flags schema{current};
      schema.set_state(new_state);
      desired = schema.raw();
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  [[nodiscard]] std::uint32_t get_numa_node() const noexcept {
    return Topology{topology.load(std::memory_order_relaxed)}.get_numa_node();
  }

  void set_numa_node(const std::uint32_t numa_id) noexcept {
    auto current = topology.load(std::memory_order_relaxed);
    std::uint64_t desired;
    do {
      Topology schema{current};
      schema.set_numa_node(numa_id);
      desired = schema.raw();
    } while (!topology.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  void set_cpu_owner(const std::uint32_t cpu_id) noexcept {
    auto current = topology.load(std::memory_order_relaxed);
    std::uint64_t desired;
    do {
      Topology schema{current};
      schema.set_cpu_id(cpu_id);
      desired = schema.raw();
    } while (!topology.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  void set_mobility(const PageMobility mobility) noexcept {
    std::uint32_t current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;
    do {
      Flags schema{current};
      schema.set_mobility(mobility);
      desired = schema.raw();
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  [[nodiscard]] PageMobility get_mobility() const noexcept {
    const Flags schema{flags.load(std::memory_order_relaxed)};
    return schema.get_mobility();
  }

  [[nodiscard]] bool try_update_slub_state(heap::SlabState &expected, const heap::SlabState desired) noexcept {
    auto old_val = std::bit_cast<std::uint64_t>(expected);
    auto new_val = std::bit_cast<std::uint64_t>(desired);

    if (slub.state.compare_exchange_weak(old_val, new_val, std::memory_order_acq_rel, std::memory_order_relaxed))
        [[likely]] {
      return true;
    }

    expected = std::bit_cast<heap::SlabState>(old_val);
    return false;
  }

  [[nodiscard]] heap::SlabState read_slub_state() const noexcept {
    return std::bit_cast<heap::SlabState>(slub.state.load(std::memory_order_relaxed));
  }
};
} // namespace kernel::memory::pmm
