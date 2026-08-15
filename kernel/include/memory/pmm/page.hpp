#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include <bitfield.hpp>

namespace kernel::memory {
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

using FlagsAndRefSchema = klib::BitfieldSchema<klib::Field<"ref_count", 0, 32>, // 0-31:  Lock-free incrementable
                                               klib::Field<"state", 32, 3>,     // 32-34: PageState enum
                                               klib::Field<"mobility", 35, 2>,  // 35-36: PageMobility enum
                                               klib::Field<"order", 37, 5>,     // 37-41: Buddy system order (0-18)
                                               klib::Field<"flags", 42, 22>     // 42-63: Hardware/Subsystem flags
                                               >;

using TopologySchema = klib::BitfieldSchema<klib::Field<"numa_node", 0, 32>, // 0-31:  Supports 2^32 NUMA nodes
                                            klib::Field<"cpu_id", 32, 32>    // 32-63: Supports 2^32 CPUs
                                            >;

struct alignas(32) Page {
  std::atomic<std::uint64_t> flags_and_ref;
  TopologySchema topology;

  union {
    Page *next;             // When Free/Standby
    std::uint64_t priv;     // When Active
    std::uint64_t swap_off; // When modified
  };

  union {
    Page *compound_head;   // When compound
    std::uint64_t mapping; // When Active
  };

  void inc_ref() noexcept {
    auto *ref_ptr = reinterpret_cast<std::atomic<std::uint32_t> *>(&flags_and_ref);
    ref_ptr->fetch_add(1, std::memory_order_relaxed);
  }

  std::uint32_t get_ref() const noexcept {
    const auto *ref_ptr = reinterpret_cast<const std::atomic<std::uint32_t> *>(&flags_and_ref);
    return ref_ptr->load(std::memory_order_relaxed);
  }

  PageState get_state() const noexcept {
    const FlagsAndRefSchema schema{
        flags_and_ref.load(std::memory_order_relaxed),
    };

    return schema.get<"state", PageState>();
  }

  void set_state(const PageState new_state) noexcept {
    std::uint64_t curr = flags_and_ref.load(std::memory_order_relaxed);
    std::uint64_t desired;

    do {
      FlagsAndRefSchema schema{curr};
      schema.set_mut<"state", PageState>(new_state);
      desired = static_cast<std::uint64_t>(schema);
    } while (!flags_and_ref.compare_exchange_weak(curr, desired, std::memory_order_release, std::memory_order_relaxed));
  }

  std::uint32_t get_numa_node() const noexcept { return topology.get<"numa_node", std::uint32_t>(); }

  void set_cpu_owner(const std::uint32_t cpu_id) noexcept { topology.set_mut<"cpu_id", std::uint32_t>(cpu_id); }
};

static_assert(sizeof(Page) == 32, "FATAL: Page crossed the 32-byte boundary!");
} // namespace kernel::memory
