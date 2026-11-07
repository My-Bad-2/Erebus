// EEVDF on its own, on fake time: every test here is a CPU simulated by hand, so what they check
// is the policy, with no timer or second CPU to blur it.
#include "sched/eevdf.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"

namespace kernel::testing {
namespace {
using sched::DEFAULT_SLICE_NS;
using sched::EevdfQueue;
using sched::Entity;
using sched::MIN_SLICE_NS;
using sched::NICE_0_WEIGHT;

constexpr std::uint64_t SLICE = DEFAULT_SLICE_NS;

// One step of a CPU: whatever the queue wants to run gets @p ns of time.
Entity* run_for(EevdfQueue& queue, const std::uint64_t ns) noexcept {
  Entity* next = queue.pick();
  if (next != queue.current()) {
    if (queue.current() != nullptr) {
      queue.requeue_current();
    }

    queue.set_current(*next);
  }

  (void)queue.charge(ns);
  return next;
}

// Takes everything off the queue again: the entities are the test's, on its stack.
template <std::size_t N>
void empty(EevdfQueue& queue, std::array<Entity, N>& entities) noexcept {
  if (queue.current() != nullptr) {
    queue.retire_current();
  }

  for (Entity& entity : entities) {
    if (entity.is_queued()) {
      queue.dequeue(entity);
    }
  }
}

[[nodiscard]] constexpr std::int64_t distance(const std::uint64_t a, const std::uint64_t b) {
  return a > b ? static_cast<std::int64_t>(a - b) : static_cast<std::int64_t>(b - a);
}
}  // namespace

KTEST(Eevdf, an_empty_queue_has_nothing_to_pick) {
  const EevdfQueue queue;

  KEXPECT(queue.pick() == nullptr);
  KEXPECT(queue.current() == nullptr);
  KEXPECT(queue.latest() == nullptr);
  KEXPECT_EQ(queue.queued(), 0u);
  KEXPECT(queue.is_consistent());
}

KTEST(Eevdf, a_heavier_entity_ages_more_slowly) {
  struct Case {
    std::uint32_t weight;
    std::uint64_t aged;
  };

  for (const Case test :
       {Case{NICE_0_WEIGHT, 4000}, Case{4 * NICE_0_WEIGHT, 1000}, Case{NICE_0_WEIGHT / 4, 16000}}) {
    EevdfQueue queue;
    Entity entity;
    entity.set_weight(test.weight);

    queue.enqueue(entity);
    queue.set_current(entity);

    const std::uint64_t before = entity.vruntime();
    (void)queue.charge(4000);
    KEXPECT_EQ(entity.vruntime() - before, test.aged);

    queue.retire_current();
  }
}

KTEST(Eevdf, a_weight_of_zero_counts_as_one) {
  Entity entity;
  entity.set_weight(0);
  KEXPECT_EQ(entity.weight(), 1u);
}

KTEST(Eevdf, slices_have_a_lower_limit) {
  Entity entity;
  entity.set_slice_ns(1);
  KEXPECT_EQ(entity.slice_ns(), MIN_SLICE_NS);

  entity.set_slice_ns(5 * MIN_SLICE_NS);
  KEXPECT_EQ(entity.slice_ns(), 5 * MIN_SLICE_NS);
}

KTEST(Eevdf, equal_weights_get_equal_time) {
  EevdfQueue queue;
  std::array<Entity, 3> entities;
  std::array<std::uint64_t, 3> received{};

  for (Entity& entity : entities) {
    queue.enqueue(entity);
  }

  for (std::uint32_t step = 0; step < 3000; ++step) {
    Entity* ran = run_for(queue, SLICE / 4);
    received[static_cast<std::size_t>(ran - entities.data())] += SLICE / 4;
  }

  // Nobody gets ahead of the others by more than a slice or so.
  for (const std::uint64_t time : received) {
    KEXPECT_LE(distance(time, 1000 * (SLICE / 4)), static_cast<std::int64_t>(2 * SLICE));
  }

  empty(queue, entities);
}

KTEST(Eevdf, time_goes_by_weight) {
  constexpr std::array<std::uint32_t, 4> WEIGHTS{256, 1024, 1024, 4096};
  constexpr std::uint32_t STEPS = 40000;

  EevdfQueue queue;
  std::array<Entity, 4> entities;
  std::array<std::uint64_t, 4> received{};
  std::uint64_t total_weight = 0;

  for (std::size_t i = 0; i < entities.size(); ++i) {
    entities[i].set_weight(WEIGHTS[i]);
    queue.enqueue(entities[i]);
    total_weight += WEIGHTS[i];
  }

  for (std::uint32_t step = 0; step < STEPS; ++step) {
    Entity* ran = run_for(queue, SLICE / 4);
    received[static_cast<std::size_t>(ran - entities.data())] += SLICE / 4;
  }

  const std::uint64_t total_time = STEPS * (SLICE / 4);
  for (std::size_t i = 0; i < entities.size(); ++i) {
    const std::uint64_t fair_share = total_time / total_weight * WEIGHTS[i];
    KEXPECT_LE(distance(received[i], fair_share), static_cast<std::int64_t>(8 * SLICE));
  }

  empty(queue, entities);
}

// What makes EEVDF more than a fair queue: a slice is about when a thread runs, and has no say in
// how much. the_earliest_deadline_goes_first has the other half of this.
KTEST(Eevdf, a_slice_of_any_length_gets_the_same_share) {
  EevdfQueue queue;
  std::array<Entity, 3> entities;
  entities[0].set_slice_ns(SLICE / 4);
  entities[1].set_slice_ns(SLICE);
  entities[2].set_slice_ns(4 * SLICE);

  for (Entity& entity : entities) {
    queue.enqueue(entity);
  }

  std::array<std::uint64_t, 3> received{};
  for (std::uint32_t step = 0; step < 24000; ++step) {
    const Entity* ran = run_for(queue, MIN_SLICE_NS);
    received[static_cast<std::size_t>(ran - entities.data())] += MIN_SLICE_NS;
  }

  for (const std::uint64_t time : received) {
    KEXPECT_LE(distance(time, 8000 * MIN_SLICE_NS), static_cast<std::int64_t>(8 * SLICE));
  }

  empty(queue, entities);
}

KTEST(Eevdf, the_earliest_deadline_goes_first) {
  EevdfQueue queue;
  Entity patient;
  Entity urgent;
  patient.set_slice_ns(4 * SLICE);
  urgent.set_slice_ns(SLICE);

  queue.enqueue(patient);
  queue.enqueue(urgent);

  KEXPECT(queue.pick() == &urgent);

  queue.dequeue(patient);
  queue.dequeue(urgent);
}

KTEST(Eevdf, an_entity_past_its_share_waits_whatever_its_deadline) {
  EevdfQueue queue;
  Entity greedy;
  Entity behind;
  greedy.set_slice_ns(MIN_SLICE_NS);
  behind.set_slice_ns(64 * SLICE);

  queue.enqueue(greedy);
  queue.enqueue(behind);

  queue.set_current(greedy);
  (void)queue.charge(MIN_SLICE_NS / 2);
  queue.requeue_current();

  KEXPECT(greedy.vdeadline() < behind.vdeadline());
  KEXPECT(!queue.is_eligible(greedy));
  KEXPECT(queue.is_eligible(behind));
  KEXPECT(queue.pick() == &behind);

  queue.dequeue(greedy);
  queue.dequeue(behind);
}

KTEST(Eevdf, pick_agrees_with_looking_at_everything) {
  constexpr std::size_t COUNT = 64;
  static std::array<Entity, COUNT> entities;

  EevdfQueue queue;
  Rng rng{0x1234};

  for (Entity& entity : entities) {
    entity.set_weight(static_cast<std::uint32_t>(64 + rng.below(8192)));
    entity.set_slice_ns(MIN_SLICE_NS + rng.below(8 * SLICE));
    queue.enqueue(entity);
  }

  for (std::uint32_t step = 0; step < 6000; ++step) {
    // Shake things up: people leave, come back and run for odd lengths of time.
    Entity& someone = entities[rng.below(COUNT)];
    if (someone.is_queued()) {
      queue.dequeue(someone);
    } else if (&someone != queue.current()) {
      queue.enqueue(someone);
    }

    if (queue.pick() != nullptr) {
      (void)run_for(queue, 1 + rng.below(2 * SLICE));
    }

    // What the walk down the tree should come up with, the slow way.
    const Entity* best = nullptr;
    for (const Entity& entity : entities) {
      if (entity.is_queued() && queue.is_eligible(entity) &&
          (best == nullptr || entity.vdeadline() < best->vdeadline())) {
        best = &entity;
      }
    }

    const Entity* running = queue.current();
    const Entity* picked = queue.pick();
    if (best != nullptr && running != nullptr && queue.is_eligible(*running) &&
        running->vdeadline() <= best->vdeadline()) {
      KEXPECT(picked == running);
    } else if (best != nullptr) {
      KREQUIRE(picked != nullptr);
      KEXPECT_EQ(picked->vdeadline(), best->vdeadline());
    } else {
      KEXPECT(picked == running);
    }

    KEXPECT(queue.is_consistent());
  }

  empty(queue, entities);
  KEXPECT(queue.pick() == nullptr);
}

KTEST(Eevdf, the_tree_stays_balanced_through_any_churn) {
  constexpr std::size_t COUNT = 1024;
  static std::array<Entity, COUNT> entities;

  EevdfQueue queue;
  Rng rng{0xBEEF};

  for (std::uint32_t step = 0; step < 40000; ++step) {
    Entity& entity = entities[rng.below(COUNT)];
    if (entity.is_queued()) {
      queue.dequeue(entity);
    } else {
      entity.set_slice_ns(MIN_SLICE_NS + rng.below(SLICE));
      queue.enqueue(entity);
    }

    if (step % 64 == 0) {
      KREQUIRE(queue.is_consistent());
    }
  }

  KEXPECT(queue.is_consistent());
  empty(queue, entities);
  KEXPECT_EQ(queue.total_weight(), 0u);
}

KTEST(Eevdf, equal_deadlines_go_in_the_order_they_came) {
  EevdfQueue queue;
  std::array<Entity, 8> entities;

  for (Entity& entity : entities) {
    queue.enqueue(entity);
  }

  for (Entity& entity : entities) {
    KEXPECT(queue.pick() == &entity);
    queue.dequeue(entity);
  }
}

KTEST(Eevdf, a_new_entity_starts_with_what_is_fair) {
  EevdfQueue queue;
  std::array<Entity, 3> entities;
  queue.enqueue(entities[0]);
  queue.enqueue(entities[1]);

  for (std::uint32_t step = 0; step < 100; ++step) {
    (void)run_for(queue, SLICE / 2);
  }

  const std::uint64_t before = queue.virtual_time();
  queue.enqueue(entities[2]);

  KEXPECT_EQ(entities[2].vruntime(), before);
  KEXPECT_EQ(queue.virtual_time(), before);
  KEXPECT_EQ(entities[2].vdeadline(), before + SLICE);

  empty(queue, entities);
}

KTEST(Eevdf, leaving_does_not_wipe_out_a_debt) {
  EevdfQueue queue;
  std::array<Entity, 3> entities;

  for (Entity& entity : entities) {
    queue.enqueue(entity);
  }

  // entities[0] gets a head start, and then blocks.
  queue.set_current(entities[0]);
  (void)queue.charge(SLICE);
  queue.retire_current();

  const std::int64_t lag = entities[0].vlag();
  KEXPECT_LT(lag, 0);

  for (std::uint32_t step = 0; step < 50; ++step) {
    (void)run_for(queue, SLICE);
  }

  queue.requeue_current();

  const std::uint64_t now = queue.virtual_time();
  queue.enqueue(entities[0]);

  // Still behind by what it left with, so it does not get to go first for having been away.
  KEXPECT(!queue.is_eligible(entities[0]));
  KEXPECT_LE(
      distance(queue.virtual_time() - static_cast<std::uint64_t>(lag), entities[0].vruntime()), 2);
  KEXPECT_GT(entities[0].vruntime(), now);

  empty(queue, entities);
}

KTEST(Eevdf, leaving_does_not_earn_anything_either) {
  EevdfQueue queue;
  std::array<Entity, 2> entities;
  queue.enqueue(entities[0]);
  queue.enqueue(entities[1]);

  // entities[1] runs, so entities[0] is owed something when it leaves.
  queue.set_current(entities[1]);
  (void)queue.charge(SLICE);
  queue.dequeue(entities[0]);

  const std::int64_t lag = entities[0].vlag();
  KEXPECT_GT(lag, 0);

  // However long it stays away, it comes back with that much, and no more.
  for (std::uint32_t step = 0; step < 1000; ++step) {
    (void)queue.charge(SLICE);
  }

  queue.enqueue(entities[0]);
  queue.dequeue(entities[0]);
  KEXPECT_LE(
      distance(static_cast<std::uint64_t>(entities[0].vlag()), static_cast<std::uint64_t>(lag)), 2);

  queue.retire_current();
}

KTEST(Eevdf, lag_has_a_limit) {
  EevdfQueue queue;
  std::array<Entity, 2> entities;
  queue.enqueue(entities[0]);
  queue.enqueue(entities[1]);

  queue.set_current(entities[0]);
  (void)queue.charge(1000 * SLICE);
  queue.retire_current();

  KEXPECT_EQ(entities[0].vlag(), -static_cast<std::int64_t>(sched::MAX_LAG_SLICES * SLICE));

  queue.dequeue(entities[1]);
}

KTEST(Eevdf, time_does_not_start_over_on_an_empty_queue) {
  EevdfQueue queue;
  Entity first;
  Entity second;

  queue.enqueue(first);
  queue.set_current(first);
  (void)queue.charge(100 * SLICE);
  const std::uint64_t reached = queue.virtual_time();
  queue.retire_current();

  KEXPECT_EQ(queue.total_weight(), 0u);
  KEXPECT_EQ(queue.virtual_time(), reached);

  queue.enqueue(second);
  KEXPECT_EQ(second.vruntime(), reached);
  queue.dequeue(second);
}

KTEST(Eevdf, a_used_up_slice_means_a_new_deadline) {
  EevdfQueue queue;
  Entity entity;
  queue.enqueue(entity);
  queue.set_current(entity);

  const std::uint64_t deadline = entity.vdeadline();

  KEXPECT(!queue.charge(SLICE - 1));
  KEXPECT_EQ(entity.vdeadline(), deadline);
  KEXPECT_EQ(queue.slice_left_ns(), 1u);

  KEXPECT(queue.charge(1));
  KEXPECT_EQ(entity.vdeadline(), deadline + SLICE);
  KEXPECT_EQ(queue.slice_left_ns(), SLICE);

  queue.retire_current();
}

KTEST(Eevdf, charging_nobody_changes_nothing) {
  EevdfQueue queue;
  Entity entity;
  queue.enqueue(entity);

  KEXPECT(!queue.charge(SLICE));
  KEXPECT_EQ(queue.virtual_time(), 0u);
  KEXPECT_EQ(queue.slice_left_ns(), 0u);

  queue.dequeue(entity);
}

KTEST(Eevdf, yielding_lets_the_others_go_first) {
  EevdfQueue queue;
  std::array<Entity, 2> entities;
  queue.enqueue(entities[0]);
  queue.enqueue(entities[1]);

  queue.set_current(entities[0]);
  KEXPECT(queue.pick() == &entities[0]);

  queue.yield_current();
  KEXPECT(queue.pick() == &entities[1]);

  queue.requeue_current();
  empty(queue, entities);
}

KTEST(Eevdf, yielding_with_nobody_around_keeps_the_cpu) {
  EevdfQueue queue;
  Entity entity;
  queue.enqueue(entity);
  queue.set_current(entity);

  queue.yield_current();
  KEXPECT(queue.pick() == &entity);
  KEXPECT_LE(queue.slice_left_ns(), SLICE);

  queue.retire_current();
}

KTEST(Eevdf, a_wakeup_only_preempts_with_an_earlier_deadline) {
  EevdfQueue queue;
  Entity running;
  Entity soon;
  Entity later;
  running.set_slice_ns(2 * SLICE);
  soon.set_slice_ns(SLICE / 2);
  later.set_slice_ns(8 * SLICE);

  queue.enqueue(running);
  queue.set_current(running);

  queue.enqueue(soon);
  queue.enqueue(later);

  KEXPECT(queue.preempts_current(soon));
  KEXPECT(!queue.preempts_current(later));

  queue.dequeue(soon);
  queue.dequeue(later);
  queue.retire_current();
}

KTEST(Eevdf, the_latest_deadlines_can_be_walked_backwards) {
  EevdfQueue queue;
  std::array<Entity, 16> entities;

  for (std::size_t i = 0; i < entities.size(); ++i) {
    entities[i].set_slice_ns(MIN_SLICE_NS * (i + 1));
    queue.enqueue(entities[i]);
  }

  std::size_t expected = entities.size();
  for (const Entity* entity = queue.latest(); entity != nullptr;
       entity = EevdfQueue::before(entity)) {
    KREQUIRE_GT(expected, 0u);
    KEXPECT(entity == &entities[--expected]);
  }

  KEXPECT_EQ(expected, 0u);

  KEXPECT(queue.find_latest([&](const Entity& e) { return &e == &entities[12]; }, 16) ==
          &entities[12]);
  KEXPECT(queue.find_latest([&](const Entity& e) { return &e == &entities[12]; }, 3) == nullptr);
  KEXPECT(queue.find_latest([](const Entity&) { return false; }, 100) == nullptr);

  empty(queue, entities);
}

// A full 128 bits for the weighted sum is not for show: this does not fit in 64.
KTEST(Eevdf, very_large_numbers_add_up) {
  constexpr std::size_t COUNT = 2048;
  static std::array<Entity, COUNT> entities;

  EevdfQueue queue;
  for (Entity& entity : entities) {
    entity.set_weight(65536);
    queue.enqueue(entity);
  }

  // Years of virtual time, and enough weight for the sum to go well past 64 bits.
  for (std::uint32_t step = 0; step < 512; ++step) {
    (void)run_for(queue, 1ull << 52);
  }

  KEXPECT(queue.is_consistent());
  KEXPECT_GT(queue.virtual_time(), 0u);
  KEXPECT(queue.pick() != nullptr);
  empty(queue, entities);
}

// What an entity is owed is w * (V - v) and has to survive this, or a thread would gain or lose
// time just by having inherited something for a moment.
KTEST(Eevdf, changing_the_weight_keeps_what_an_entity_is_owed) {
  EevdfQueue queue;
  std::array<Entity, 4> entities;

  for (Entity& entity : entities) {
    queue.enqueue(entity);
  }

  // Not a whole number of slices each, or everyone is back to owing nothing, and scaling that
  // proves nothing.
  for (std::uint32_t round = 0; round < 14; ++round) {
    (void)run_for(queue, SLICE / 3);
  }

  if (queue.current() != nullptr) {
    queue.requeue_current();
  }

  Entity& behind = *std::ranges::min_element(entities, {}, &Entity::vruntime);
  const std::uint64_t virtual_time = queue.virtual_time();
  const auto lag = static_cast<std::int64_t>(virtual_time - behind.vruntime());
  const std::uint64_t left = behind.vdeadline() - behind.vruntime();

  KREQUIRE_GT(lag, 0);

  queue.reweight(behind, NICE_0_WEIGHT * 4);

  KEXPECT_EQ(behind.weight(), NICE_0_WEIGHT * 4);
  KEXPECT_LE(distance(queue.virtual_time(), virtual_time), 2);
  KEXPECT_LE(distance(static_cast<std::uint64_t>(lag),
                      4 * static_cast<std::uint64_t>(virtual_time - behind.vruntime())),
             8);
  KEXPECT_LE(distance(left, 4 * (behind.vdeadline() - behind.vruntime())), 8);
  KEXPECT(queue.is_consistent());

  queue.reweight(behind, NICE_0_WEIGHT);
  KEXPECT_LE(distance(queue.virtual_time(), virtual_time), 2);
  KEXPECT(queue.is_consistent());

  empty(queue, entities);
}

KTEST(Eevdf, the_running_entity_can_change_its_weight) {
  EevdfQueue queue;
  Entity running;
  Entity waiting;

  queue.enqueue(running);
  queue.enqueue(waiting);
  queue.set_current(running);
  (void)queue.charge(SLICE / 2);

  const std::uint64_t left = queue.slice_left_ns();
  queue.reweight(running, NICE_0_WEIGHT * 8);

  KEXPECT_EQ(running.weight(), NICE_0_WEIGHT * 8);
  KEXPECT(queue.current() == &running);
  KEXPECT_LE(distance(queue.slice_left_ns(), left), 16);
  KEXPECT(queue.is_consistent());

  // Eight times the weight ages at an eighth of the speed, which is what it should do from here.
  const std::uint64_t before = running.vruntime();
  (void)queue.charge(8 * 1024);
  KEXPECT_EQ(running.vruntime() - before, 1024ull);

  queue.retire_current();
  queue.dequeue(waiting);
}

KTEST(Eevdf, the_only_entity_on_a_queue_can_change_its_weight) {
  EevdfQueue queue;
  Entity entity;

  queue.enqueue(entity);
  queue.reweight(entity, 3 * NICE_0_WEIGHT);

  KEXPECT_EQ(queue.total_weight(), 3ull * NICE_0_WEIGHT);
  KEXPECT(queue.pick() == &entity);
  KEXPECT(queue.is_consistent());

  queue.dequeue(entity);
}

KTEST(Eevdf, the_order_of_the_tree_follows_a_new_weight) {
  EevdfQueue queue;
  Entity first;
  Entity second;
  second.set_slice_ns(4 * SLICE);

  queue.enqueue(first);
  queue.enqueue(second);

  KEXPECT(queue.pick() == &first);

  // Far heavier, its request comes to a much nearer virtual deadline.
  queue.reweight(second, NICE_0_WEIGHT * 64);

  KEXPECT(queue.pick() == &second);
  KEXPECT(queue.is_consistent());

  queue.dequeue(first);
  queue.dequeue(second);
}

KTEST(Eevdf, shares_follow_a_change_of_weight) {
  EevdfQueue queue;
  Entity light;
  Entity heavy;

  queue.enqueue(light);
  queue.enqueue(heavy);

  for (std::uint32_t round = 0; round < 100; ++round) {
    (void)run_for(queue, SLICE);
  }

  queue.reweight(heavy, NICE_0_WEIGHT * 3);

  std::uint64_t light_ns = 0;
  std::uint64_t heavy_ns = 0;
  for (std::uint32_t round = 0; round < 4000; ++round) {
    (run_for(queue, SLICE / 4) == &light ? light_ns : heavy_ns) += SLICE / 4;
  }

  KEXPECT_LE(distance(heavy_ns, 3 * light_ns), (heavy_ns + light_ns) / 16);

  if (queue.current() != nullptr) {
    queue.retire_current();
  }

  for (Entity* entity : {&light, &heavy}) {
    if (entity->is_queued()) {
      queue.dequeue(*entity);
    }
  }
}

// A wrong sum shows up nowhere at first, and much later as a pick that finds nothing eligible.
KTEST(Eevdf, survives_random_changes_of_weight) {
  static constexpr std::uint32_t COUNT = 32;

  EevdfQueue queue;
  std::array<Entity, COUNT> entities;
  support::Rng rng{7};

  for (Entity& entity : entities) {
    entity.set_weight(static_cast<std::uint32_t>(64 + rng.below(8192)));
    queue.enqueue(entity);
  }

  for (std::uint32_t round = 0; round < 20'000; ++round) {
    (void)run_for(queue, 1 + rng.below(SLICE));

    Entity& someone = entities[rng.below(COUNT)];
    const auto weight = static_cast<std::uint32_t>(1 + rng.below(70'000));

    if (someone.is_queued() || &someone == queue.current()) {
      queue.reweight(someone, weight);
    } else {
      queue.enqueue(someone);
    }

    if (rng.below(8) == 0 && queue.queued() > 1) {
      Entity& leaver = entities[rng.below(COUNT)];
      if (leaver.is_queued()) {
        queue.dequeue(leaver);
      }
    }

    if (round % 1024 == 0) {
      KREQUIRE(queue.is_consistent());
    }
  }

  KEXPECT(queue.is_consistent());
  empty(queue, entities);
}

KTEST(Eevdf, an_entity_off_the_queue_keeps_what_it_is_owed_too) {
  EevdfQueue queue;
  Entity leaver;
  Entity other;

  queue.enqueue(leaver);
  queue.enqueue(other);

  queue.set_current(leaver);
  (void)queue.charge(SLICE);
  queue.retire_current();

  const std::int64_t lag = leaver.vlag();
  KEXPECT_LT(lag, 0);

  leaver.set_weight(NICE_0_WEIGHT * 2);
  KEXPECT_EQ(leaver.vlag(), lag / 2);

  queue.dequeue(other);
}

}  // namespace kernel::testing
