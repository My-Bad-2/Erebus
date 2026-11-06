/// @file eevdf.hpp
/// @brief Earliest Eligible Virtual Deadline First, the policy for ordinary threads.
///
/// EEVDF (Stoica and Abdel-Wahab, 1995) shares a CPU by asking two questions about every thread:
/// has it received less than its share so far, and of the ones that have, whose request is due
/// first? In terms of the numbers kept here:
///
///  - Every entity has a *virtual runtime*: nanoseconds of CPU it received, scaled down by its
///    weight. A thread of twice the weight ages half as fast, so equal virtual runtimes mean
///    everyone got what their weight says they should.
///  - The queue's *virtual time* V is the weight-averaged virtual runtime of everything on it.
///    That makes V the point where a thread has received exactly its share.
///  - An entity is *eligible* when its virtual runtime is not past V, that is, when it is owed
///    CPU time rather than owing it. The difference, V - vruntime, is its *lag*.
///  - Every entity also has a *virtual deadline*: its virtual runtime plus one slice, scaled by
///    its weight. A shorter slice means an earlier deadline, so slice length is how a thread
///    trades throughput for latency, and has nothing to do with its share.
///
/// The next entity to run is the eligible one with the earliest deadline. Lag is what makes this
/// honest across sleeps: an entity that leaves the queue remembers its lag and comes back with
/// it, so blocking does not wipe out a debt, and being away does not earn anything new.
///
/// Everything here is arithmetic on a tree, with no locks, clocks or threads anywhere near it. It
/// runs just as well on fake time, which is how the tests get at it, and the caller decides what
/// protects a queue. See RunQueue for that.
///
/// ### The tree
///
/// Queued entities sit in a utils::AvlTree sorted by deadline, with every node also holding the
/// lowest virtual runtime in its subtree. That one number answers "is anything down there
/// eligible?" without going down there, so the earliest eligible deadline takes one walk from root
/// to leaf.
///
/// The nodes live in the entities themselves. A scheduler that has to allocate can run out of
/// memory while waking a thread, which leaves nothing sensible to do, and would make the heap
/// unable to wake anyone while it holds its own locks. Here, queueing cannot fail.
#pragma once

#include <cstddef>
#include <cstdint>

#include "utils/avl_tree.hpp"

namespace kernel::sched {
/// @brief Weight of a normal thread: the one whose virtual time runs at wall clock speed.
inline constexpr std::uint32_t NICE_0_WEIGHT = 1024;

/// @brief How long an entity runs before its deadline moves, unless it asks for something else.
inline constexpr std::uint64_t DEFAULT_SLICE_NS = 2'000'000;

/// @brief Shortest slice there is. Anything shorter would spend more time switching than running.
inline constexpr std::uint64_t MIN_SLICE_NS = 250'000;

/// @brief How many slices of lag an entity can carry away from a queue, in either direction.
inline constexpr std::uint64_t MAX_LAG_SLICES = 2;

/// @brief What the scheduler knows about something that competes for a CPU.
///
/// sched::Thread derives from this. All of the virtual state belongs to the EevdfQueue the entity
/// is on, and only weight and slice are for anyone else to set, while it is on no queue.
class Entity : public utils::AvlNode {
  friend class EevdfQueue;

  // Fixed point format of m_rate.
  static constexpr std::uint32_t RATE_SHIFT = 32;

  std::uint64_t m_subtree_min_vruntime{0};

  std::uint32_t m_weight{NICE_0_WEIGHT};
  std::uint64_t m_rate{std::uint64_t{1} << RATE_SHIFT};  ///< 32.32 fixed point NICE_0 / weight.
  std::uint64_t m_slice_ns{DEFAULT_SLICE_NS};

  std::uint64_t m_vruntime{0};
  std::uint64_t m_vdeadline{0};
  std::int64_t m_vlag{0};  ///< What the entity left its last queue with.

  // A multiplication instead of a division by the weight, as this runs on every charge.
  [[nodiscard]] constexpr std::uint64_t to_virtual(const std::uint64_t ns) const noexcept {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(ns) * m_rate) >> RATE_SHIFT);
  }

  [[nodiscard]] constexpr std::uint64_t to_real(const std::uint64_t virtual_ns) const noexcept {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(virtual_ns) * m_weight) /
                                      NICE_0_WEIGHT);
  }

 public:
  constexpr Entity() noexcept = default;

  Entity(const Entity&) = delete;
  Entity& operator=(const Entity&) = delete;

  [[nodiscard]] constexpr std::uint32_t weight() const noexcept { return m_weight; }
  [[nodiscard]] constexpr std::uint64_t slice_ns() const noexcept { return m_slice_ns; }
  [[nodiscard]] constexpr std::uint64_t vruntime() const noexcept { return m_vruntime; }
  [[nodiscard]] constexpr std::uint64_t vdeadline() const noexcept { return m_vdeadline; }
  [[nodiscard]] constexpr std::int64_t vlag() const noexcept { return m_vlag; }

  /// @brief Sets the share of the entity. A weight of 0 would never age, so it counts as 1.
  /// @pre The entity is on no queue.
  constexpr void set_weight(const std::uint32_t weight) noexcept {
    m_weight = weight != 0 ? weight : 1;
    m_rate = (std::uint64_t{NICE_0_WEIGHT} << RATE_SHIFT) / m_weight;
  }

  /// @brief Sets how much CPU time the entity asks for at once, at least MIN_SLICE_NS.
  /// @pre The entity is on no queue.
  constexpr void set_slice_ns(const std::uint64_t slice_ns) noexcept {
    m_slice_ns = slice_ns >= MIN_SLICE_NS ? slice_ns : MIN_SLICE_NS;
  }

  /// @brief Whether the entity is waiting in a queue's tree. The running one is not.
  [[nodiscard]] constexpr bool is_queued() const noexcept { return in_tree(); }
};

/// @brief The entities competing for one CPU.
///
/// At most one of them runs at a time: it is the queue's *current* entity. It stays part of the
/// average while it runs but leaves the tree, as its virtual runtime keeps moving and that would
/// wreck the tree's order. A thread goes through these steps:
///
/// @code
/// queue.enqueue(entity);          // joins: placed according to its lag
/// queue.set_current(*queue.pick());
/// queue.charge(ran_for_ns);       // as often as there is news
/// queue.requeue_current();        // preempted, stays in the competition
/// queue.retire_current();         // blocked or gone: leaves, taking its lag along
/// @endcode
///
/// Not thread safe; see the file comment. Virtual runtimes are not compared with wraparound in
/// mind, as 64 bits of nanoseconds last 146 years of CPU time even at a weight of 256.
class EevdfQueue {
  // By deadline, each node also knowing the lowest virtual runtime below it.
  struct TreeTraits {
    [[nodiscard]] static bool less(const Entity& a, const Entity& b) noexcept {
      return a.m_vdeadline < b.m_vdeadline;
    }

    static void update(Entity& entity) noexcept;
  };

  using Tree = utils::AvlTree<Entity, TreeTraits>;

  Tree m_tree{};
  Entity* m_current{nullptr};

  // Sum of weight * vruntime over the tree and the current entity, the numerator of V. A full 128
  // bits, since that is what the product of a large weight and a large virtual runtime can reach.
  unsigned __int128 m_weighted_vruntime{0};
  std::uint64_t m_total_weight{0};
  std::uint32_t m_queued{0};

  // V for when nobody is here to define it, so that time does not start over on an empty queue.
  std::uint64_t m_last_virtual_time{0};

  [[nodiscard]] bool is_eligible_at(std::uint64_t vruntime) const noexcept;

  void account_join(const Entity& entity) noexcept;
  void account_leave(Entity& entity) noexcept;

 public:
  constexpr EevdfQueue() noexcept = default;

  EevdfQueue(const EevdfQueue&) = delete;
  EevdfQueue& operator=(const EevdfQueue&) = delete;

  /// @brief V, where an entity has received exactly its share.
  [[nodiscard]] std::uint64_t virtual_time() const noexcept;

  /// @brief Whether @p entity has received no more than its share so far.
  [[nodiscard]] bool is_eligible(const Entity& entity) const noexcept;

  /// @brief Adds @p entity to the competition, placed so that it has the lag it left with.
  void enqueue(Entity& entity) noexcept;

  /// @brief Takes a waiting entity out of the competition. It keeps its lag for the next enqueue().
  void dequeue(Entity& entity) noexcept;

  /// @brief Makes the waiting entity @p entity the running one.
  /// @pre Nothing is running.
  void set_current(Entity& entity) noexcept;

  /// @brief Puts the running entity back among the waiting ones, its deadline unchanged.
  void requeue_current() noexcept;

  /// @brief Takes the running entity out of the competition, as dequeue() does a waiting one.
  void retire_current() noexcept;

  /// @brief Charges the running entity for @p delta_ns of CPU time. Once it has used up its slice,
  ///        it gets its next one, with a new deadline.
  /// @return Whether that happened.
  bool charge(std::uint64_t delta_ns) noexcept;

  /// @brief Lets the others go first: the running entity moves on to its next deadline.
  void yield_current() noexcept;

  /// @brief The entity that should be running now, which may be the current one.
  /// @return Null only if there is nothing at all on the queue.
  [[nodiscard]] Entity* pick() const noexcept;

  /// @brief Whether the newly enqueued @p entity should take over from the current one.
  [[nodiscard]] bool preempts_current(const Entity& entity) const noexcept;

  /// @brief What is left of the running entity's slice, in real time.
  [[nodiscard]] std::uint64_t slice_left_ns() const noexcept;

  [[nodiscard]] Entity* current() const noexcept { return m_current; }

  /// @brief Number of entities waiting, not counting the running one.
  [[nodiscard]] std::uint32_t queued() const noexcept { return m_queued; }

  /// @brief Sum of the weights of everything here, the running entity included.
  [[nodiscard]] std::uint64_t total_weight() const noexcept { return m_total_weight; }

  /// @brief The waiting entity with the latest deadline, the one that has the longest to wait.
  [[nodiscard]] Entity* latest() const noexcept { return m_tree.last(); }

  /// @brief The waiting entity due just before @p entity.
  [[nodiscard]] static Entity* before(const Entity* entity) noexcept { return Tree::prev(*entity); }

  /// @brief Finds the latest waiting entity for which `pred(entity)` holds, looking at no more than
  ///        @p budget of them.
  template <typename Pred>
  [[nodiscard]] Entity* find_latest(Pred&& pred, std::uint32_t budget) const noexcept {
    for (Entity* entity = latest(); entity != nullptr && budget > 0;
         entity = before(entity), --budget) {
      if (pred(*entity)) {
        return entity;
      }
    }

    return nullptr;
  }

  /// @brief Checks every invariant of the tree and the running totals. O(n), for tests.
  [[nodiscard]] bool is_consistent() const noexcept;
};
}  // namespace kernel::sched
