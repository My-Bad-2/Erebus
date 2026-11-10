#include "core/cap.hpp"

#include <atomic>

#include "core/cspace.hpp"
#include "sched/workqueue.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::core::ipc {
namespace {
void run_reclaim(void*) noexcept { reclaim_now(); }

constinit std::atomic<CapObject*> g_dead{nullptr};
constinit sched::WorkItem g_reclaim{&run_reclaim};
}  // namespace

void CapObject::release_ref() noexcept {
  [[maybe_unused]] const std::uint32_t previous = m_refs.fetch_sub(1, std::memory_order_acq_rel);
  KASSERT(previous > 0);

  if (previous == 1) {
    utils::rcu::call_after_grace_period(&CapObject::after_grace_period, this);
  }
}

void CapObject::after_grace_period(void* object) noexcept {
  // In a deferred call, where nothing may sleep, and a destructor may want to. On to the worker.
  auto* self = static_cast<CapObject*>(object);
  CapObject* head = g_dead.load(std::memory_order_relaxed);
  do {
    self->m_next_dead = head;
  } while (!g_dead.compare_exchange_weak(head, self, std::memory_order_release,
                                         std::memory_order_relaxed));

  // False before the workqueues are up, or when the worker is due anyway: either way, whatever
  // runs next takes this along.
  (void)sched::workqueue::queue(g_reclaim);
}

void reclaim_now() noexcept {
  CapObject* dead = g_dead.exchange(nullptr, std::memory_order_acquire);
  while (dead != nullptr) {
    CapObject* next = dead->m_next_dead;
    dead->m_destroy(dead);
    dead = next;
  }
}

void initialize() noexcept {
  CSpace::initialize_cache();
  (void)sched::workqueue::queue(g_reclaim);
}
}  // namespace kernel::core::ipc
