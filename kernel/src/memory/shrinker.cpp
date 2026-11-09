#include "memory/shrinker.hpp"

#include "utils/lock.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/srcu.hpp"

namespace kernel::memory {
namespace {
// Keeps register_shrinker() and unregister_shrinker() out of each other's way. Walks never take it.
constinit utils::QSpinlock g_lock{};
constinit utils::rcu::Ptr<Shrinker> g_shrinkers{};

// Walks are read sections of this domain. It stays on the counters it starts with, shared by every
// CPU, and is never initialize()d: its readers are the reclaim daemons, one per node and rarely
// more than one at a time, which counters of their own would not make any faster.
constinit utils::rcu::SleepableDomain g_walks{};
}  // namespace

void register_shrinker(Shrinker& shrinker) noexcept {
  utils::IrqSaveGuard guard{g_lock};

  // Linked in before it is published, with a release, so a walk that finds it finds all of it.
  shrinker.m_next.store(g_shrinkers.load_writer_locked());
  g_shrinkers.store(&shrinker);
}

void unregister_shrinker(Shrinker& shrinker) noexcept {
  {
    utils::IrqSaveGuard guard{g_lock};

    utils::rcu::Ptr<Shrinker>* link = &g_shrinkers;
    for (Shrinker* at = link->load_writer_locked(); at != &shrinker;
         at = link->load_writer_locked()) {
      if (at == nullptr) [[unlikely]] {
        utils::logger::fatal("Reclaim: unregistering a shrinker that is not registered\n");
      }

      link = &at->m_next;
    }

    // A walk that is at it right now goes on from it to the rest, so its own link stays.
    link->store(shrinker.m_next.load_writer_locked());
  }

  // A walk that can still call it got to it before it was unlinked, so it started before, and
  // this waits for every walk that did.
  g_walks.synchronize();
}

std::size_t run_shrinkers(const std::uint32_t node_id) noexcept {
  std::size_t released = 0;

  utils::rcu::SleepableReadGuard walk{g_walks};
  for (Shrinker* shrinker = g_shrinkers.load(walk); shrinker != nullptr;
       shrinker = shrinker->m_next.load(walk)) {
    released += shrinker->m_callback(node_id, shrinker->m_context);
  }

  return released;
}
}  // namespace kernel::memory
