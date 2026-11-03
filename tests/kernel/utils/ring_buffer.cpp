#include "utils/ring_buffer.hpp"

#include <array>
#include <atomic>
#include <new>

#include "hal/hal.hpp"
#include "ktest.hpp"

namespace kernel::testing {
namespace {
using utils::DequeError;

// Counts live objects, so that leaks and double destruction show up.
struct Tracked {
  static inline std::int64_t live = 0;
  std::uint64_t value;

  explicit Tracked(const std::uint64_t v) noexcept : value(v) { ++live; }
  Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
  Tracked& operator=(Tracked&& other) noexcept {
    value = other.value;
    return *this;
  }
  ~Tracked() noexcept { --live; }
};
}  // namespace

KTEST(RingDeque, pushes_and_pops_at_both_ends) {
  utils::RingDeque<int, 4> deque;
  KEXPECT(deque.empty());
  KREQUIRE(deque.emplace_back(2).has_value());
  KREQUIRE(deque.emplace_back(3).has_value());
  KREQUIRE(deque.emplace_front(1).has_value());
  KREQUIRE(deque.emplace_back(4).has_value());
  KEXPECT(deque.full());
  KEXPECT_EQ(deque.emplace_back(5).error(), DequeError::Full);

  KEXPECT_EQ(deque.front(), 1);
  KEXPECT_EQ(deque.back(), 4);
  KEXPECT_EQ(*deque.pop_front(), 1);
  KEXPECT_EQ(*deque.pop_back(), 4);
  KEXPECT_EQ(deque.size(), 2u);
  KEXPECT_EQ(*deque.pop_front(), 2);
  KEXPECT_EQ(*deque.pop_front(), 3);
  KEXPECT_EQ(deque.pop_front().error(), DequeError::Empty);
}

KTEST(RingDeque, wraps_around_many_times) {
  utils::RingDeque<int, 8> deque;
  int next_in = 0;
  int next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    while (!deque.full()) {
      KREQUIRE(deque.emplace_back(next_in++).has_value());
    }

    for (int i = 0; i < 5; ++i) {
      KREQUIRE_EQ(*deque.pop_front(), next_out++);
    }
  }
}

KTEST(RingDeque, moves_in_bulk) {
  utils::RingDeque<int, 8> deque;
  const std::array<int, 10> in{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  KEXPECT_EQ(deque.push_back_bulk(in), 8u);

  std::array<int, 5> out{};
  KEXPECT_EQ(deque.pop_front_bulk(out), 5u);
  KEXPECT((out == std::array{0, 1, 2, 3, 4}));
  KEXPECT_EQ(deque.size(), 3u);
}

KTEST(RingBuffer, keeps_fifo_order_up_to_its_capacity) {
  utils::MpmcRingBuffer<int, 16> ring;
  for (int i = 0; i < 16; ++i) {
    KREQUIRE(ring.try_enqueue(i));
  }

  KEXPECT(!ring.try_enqueue(99));
  KEXPECT(ring.full_approx());

  int value = -1;
  for (int i = 0; i < 16; ++i) {
    KREQUIRE(ring.try_dequeue(value));
    KEXPECT_EQ(value, i);
  }

  KEXPECT(!ring.try_dequeue(value));
  KEXPECT(ring.empty_approx());
}

KTEST(RingBuffer, destroys_what_it_still_holds) {
  {
    utils::MpscRingBuffer<Tracked, 8, false> ring;
    for (std::uint64_t i = 0; i < 8; ++i) {
      KEXPECT(ring.try_emplace(i));
    }
    KEXPECT(!ring.try_emplace(99));

    Tracked out{0};
    KEXPECT(ring.try_dequeue(out));
    KEXPECT_EQ(out.value, 0u);
  }

  KEXPECT_EQ(Tracked::live, 0);
}

namespace {
constexpr std::uint32_t PER_PRODUCER = 20'000;
using SharedRing = utils::MpscRingBuffer<std::uint32_t, 64>;
std::atomic<SharedRing*> g_ring{nullptr};
}  // namespace

// CPU 0 consumes, everybody else produces. Each producer's values must come out in the order it
// put them in, and none may get lost.
KTEST_ALL_CPUS(RingBuffer, keeps_each_producers_order_across_cpus, cpu) {
  if (cpu_count() < 2) {
    KTEST_SKIP("needs a second CPU to produce");
  }

  if (cpu == 0) {
    g_ring.store(new SharedRing, std::memory_order_release);
  }
  sync_cpus();
  SharedRing& ring = *g_ring.load(std::memory_order_acquire);

  if (cpu != 0) {
    for (std::uint32_t i = 0; i < PER_PRODUCER; ++i) {
      while (!ring.try_enqueue(cpu * PER_PRODUCER + i)) {
        hw::pause();
      }
    }
  } else {
    // The consumer must keep going whatever it finds, or the producers wait for room forever.
    auto* next = new std::uint32_t[cpu_count()]{};
    bool in_order = true;
    std::uint32_t value = 0;
    for (std::uint32_t received = 0; received < (cpu_count() - 1) * PER_PRODUCER;) {
      if (!ring.try_dequeue(value)) {
        hw::pause();
        continue;
      }

      const std::uint32_t producer = value / PER_PRODUCER;
      in_order = in_order && producer < cpu_count() && value % PER_PRODUCER == next[producer];
      next[producer < cpu_count() ? producer : 0] = value % PER_PRODUCER + 1;
      ++received;
    }

    KEXPECT(in_order);
    KEXPECT(ring.empty_approx());
    delete[] next;
  }

  sync_cpus();
  if (cpu == 0) {
    delete g_ring.exchange(nullptr, std::memory_order_acq_rel);
  }
}
}  // namespace kernel::testing
