#pragma once

#include <atomic>
#include <cstdint>

#include <bitfield.hpp>

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

using FlagsSchema = klib::BitfieldSchema<klib::Field<"state", 0, 3>,    // 0-2 : PageState enum
                                         klib::Field<"mobility", 3, 2>, // 3-4 : PageMobility enum
                                         klib::Field<"order", 5, 5>,    // 5-9 : Buddy system order (0-18)
                                         klib::Field<"flags", 10, 22>   // 10-31: Hardware/Subsystem flags
                                         >;

using TopologySchema = klib::BitfieldSchema<klib::Field<"numa_node", 0, 32>, // 0-31:  Supports 2^32 NUMA nodes
                                            klib::Field<"cpu_id", 32, 32>    // 32-63: Supports 2^32 CPUs
                                            >;

struct alignas(32) Page {
  std::atomic<std::uint32_t> ref_count;
  std::atomic<std::uint32_t> flags;
  TopologySchema topology;

  union {
    struct {
      Page *next;
      std::uint64_t aba_counter;
    } freelist;

    struct {
      Page *next;
      Page *prev;
    } buddy;
  };

  void inc_ref() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] bool dec_ref_and_test() noexcept { return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1; }
  std::uint32_t get_ref() const noexcept { return ref_count.load(std::memory_order_relaxed); }

  void set_order(const std::uint8_t order) noexcept {
    auto current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;

    do {
      FlagsSchema schema{current};
      schema.set_mut<"order">(order);
      desired = static_cast<std::uint32_t>(schema);
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  std::uint8_t get_order() const noexcept {
    const FlagsSchema schema{flags.load(std::memory_order_relaxed)};
    return schema.get<"order">();
  }

  PageState get_state() const noexcept {
    const FlagsSchema schema{flags.load(std::memory_order_relaxed)};
    return schema.get<"state", PageState>();
  }

  void set_state(PageState new_state) noexcept {
    auto current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;

    do {
      FlagsSchema schema{current};
      schema.set_mut<"state">(static_cast<std::uint32_t>(new_state));
      desired = static_cast<std::uint32_t>(schema);
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  void set_mobility(const PageMobility mobility) noexcept {
    auto current = flags.load(std::memory_order_relaxed);
    std::uint32_t desired;

    do {
      FlagsSchema schema{current};
      schema.set_mut<"mobility">(mobility);
      desired = static_cast<std::uint32_t>(schema);
    } while (!flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  PageMobility get_mobility() const noexcept {
    const FlagsSchema schema{flags.load(std::memory_order_relaxed)};
    return schema.get<"mobility", PageMobility>();
  }

  std::uint32_t get_numa_node() const noexcept { return topology.get<"numa_node", std::uint32_t>(); }
  void set_cpu_owner(const std::uint32_t cpu_id) noexcept { topology.set_mut<"cpu_id", std::uint32_t>(cpu_id); }
};
} // namespace kernel::memory::pmm
