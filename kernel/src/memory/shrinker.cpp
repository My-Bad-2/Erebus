#include "memory/shrinker.hpp"

#include "sched/wait_queue.hpp"
#include "utils/lock.hpp"

namespace kernel::memory {
namespace {
constinit utils::QSpinlock g_lock{};
constinit utils::SList<Shrinker> g_shrinkers{};
constinit sched::WaitQueue g_leaving{};
}  // namespace

void register_shrinker(Shrinker& shrinker) noexcept {
  utils::IrqSaveGuard guard{g_lock};
  g_shrinkers.push_front(shrinker);
}

void unregister_shrinker(Shrinker& shrinker) noexcept {
  while (true) {
    {
      utils::IrqSaveGuard guard{g_lock};
      if (shrinker.m_running == 0) {
        (void)g_shrinkers.remove(shrinker);
        return;
      }
    }

    g_leaving.wait([&] {
      utils::IrqSaveGuard guard{g_lock};
      return shrinker.m_running == 0;
    });
  }
}

std::size_t run_shrinkers(const std::uint32_t node_id) noexcept {
  std::size_t released = 0;
  Shrinker* previous = nullptr;

  while (true) {
    Shrinker* next = nullptr;
    bool done_with_previous = false;
    {
      utils::IrqSaveGuard guard{g_lock};

      // The one just called is still on the list, as it was marked as running, so the walk can
      // go on from it however the list changed in the meantime.
      auto it = previous != nullptr ? ++g_shrinkers.iterator_to(*previous) : g_shrinkers.begin();
      if (previous != nullptr) {
        done_with_previous = --previous->m_running == 0;
      }

      if (it != g_shrinkers.end()) {
        next = &*it;
        ++next->m_running;
      }
    }

    if (done_with_previous && g_leaving.waiters() != 0) {
      (void)g_leaving.wake_all();
    }

    if (next == nullptr) {
      return released;
    }

    released += next->m_callback(node_id, next->m_context);
    previous = next;
  }
}
}  // namespace kernel::memory
