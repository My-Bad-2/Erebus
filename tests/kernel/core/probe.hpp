// A capability object for tests: it counts how often it ends and how often it becomes unreachable,
// and carries a magic word that is gone once it is destroyed, so that a lookup that hands out a
// dead object is caught the moment anyone looks at it.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "core/cap.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::testing::core_probe {
namespace ipc = core::ipc;

// A probe ends in a kworker, a grace period after its last reference goes, and counts that here.
// So whatever holds the counters has to outlive every probe: a test that keeps them in its frame
// lets go of all of it and settle()s before it returns, or the count lands in the next test's.
struct Counters {
  std::atomic<std::uint32_t> destroyed{0};
  std::atomic<std::uint32_t> unreachable{0};
};

// The type tag is one these tests never look up for real.
struct Probe final : ipc::TypedCapObject<Probe, ipc::CapType::DmaPool> {
  static constexpr std::uint64_t ALIVE = 0x5052'4F42'4541'4C49ull;

  std::atomic<std::uint64_t> magic{ALIVE};
  Counters* counters;
  std::uint64_t serial;  ///< Never the same for two probes of a test, so a recycled one shows.

  explicit Probe(Counters& c, const std::uint64_t s = 0) noexcept : counters(&c), serial(s) {}

  ~Probe() noexcept {
    magic.store(0, std::memory_order_relaxed);
    counters->destroyed.fetch_add(1, std::memory_order_acq_rel);
  }

  void on_unreachable() noexcept { counters->unreachable.fetch_add(1, std::memory_order_acq_rel); }

  [[nodiscard]] bool alive() const noexcept {
    return magic.load(std::memory_order_relaxed) == ALIVE;
  }
};

// Everything released so far is destroyed by the time this returns, or the wait times out.
[[nodiscard]] inline bool settle(const Counters& counters, const std::uint32_t destroyed) noexcept {
  utils::rcu::barrier();
  ipc::reclaim_now();
  return support::wait_for([&] {
    ipc::reclaim_now();
    return counters.destroyed.load(std::memory_order_acquire) >= destroyed;
  });
}
}  // namespace kernel::testing::core_probe
