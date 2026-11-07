#include "sched/eevdf.hpp"

#include <algorithm>

#include "utils/assert.hpp"

namespace kernel::sched {
std::uint64_t EevdfQueue::virtual_time() const noexcept {
  if (m_total_weight == 0) {
    return m_last_virtual_time;
  }

  return static_cast<std::uint64_t>(m_weighted_vruntime / m_total_weight);
}

// vruntime <= V with V written out and multiplied through, which needs no division and has no
// rounding to get wrong: the lowest virtual runtime on a queue is always eligible, exactly.
bool EevdfQueue::is_eligible_at(std::uint64_t vruntime) const noexcept {
  return static_cast<unsigned __int128>(m_total_weight) * vruntime <= m_weighted_vruntime;
}

bool EevdfQueue::is_eligible(const Entity& entity) const noexcept {
  return is_eligible_at(entity.m_vruntime);
}

void EevdfQueue::account_join(const Entity& entity) noexcept {
  m_weighted_vruntime += static_cast<unsigned __int128>(entity.m_weight) * entity.m_vruntime;
  m_total_weight += entity.m_weight;
}

void EevdfQueue::account_leave(Entity& entity) noexcept {
  const std::uint64_t now = virtual_time();
  const auto limit =
      static_cast<std::int64_t>(entity.to_virtual(MAX_LAG_SLICES * entity.m_slice_ns));
  const std::int64_t lag =
      static_cast<std::int64_t>(now) - static_cast<std::int64_t>(entity.m_vruntime);

  entity.m_vlag = std::clamp(lag, -limit, limit);
  m_weighted_vruntime -= static_cast<unsigned __int128>(entity.m_weight) * entity.m_vruntime;
  m_total_weight -= entity.m_weight;
  m_last_virtual_time = now;
}

void EevdfQueue::enqueue(Entity& entity) noexcept {
  KASSERT(!entity.is_queued() && &entity != m_current);

  const std::uint64_t now = virtual_time();

  // Joining drags V towards the newcomer, which would shrink the very lag being restored. Scaling
  // the lag up by (W + w) / W leaves it with exactly what it had once it is in the average.
  const std::uint64_t magnitude = entity.m_vlag < 0 ? -static_cast<std::uint64_t>(entity.m_vlag)
                                                    : static_cast<std::uint64_t>(entity.m_vlag);
  const std::uint64_t lag =
      m_total_weight == 0
          ? magnitude
          : static_cast<std::uint64_t>(static_cast<unsigned __int128>(magnitude) *
                                       (m_total_weight + entity.m_weight) / m_total_weight);

  if (entity.m_vlag < 0) {
    entity.m_vruntime = now + lag;
  } else {
    entity.m_vruntime = now - std::min(now, lag);
  }

  entity.m_vdeadline = entity.m_vruntime + entity.to_virtual(entity.m_slice_ns);

  account_join(entity);
  m_tree.insert(entity);
  ++m_queued;
}

void EevdfQueue::dequeue(Entity& entity) noexcept {
  KASSERT(entity.is_queued());

  m_tree.erase(entity);
  --m_queued;
  account_leave(entity);
}

void EevdfQueue::set_current(Entity& entity) noexcept {
  KASSERT(m_current == nullptr && entity.is_queued());

  m_tree.erase(entity);
  --m_queued;
  m_current = &entity;
}

void EevdfQueue::requeue_current() noexcept {
  KASSERT(m_current != nullptr);

  m_tree.insert(*m_current);
  ++m_queued;
  m_current = nullptr;
}

void EevdfQueue::retire_current() noexcept {
  KASSERT(m_current != nullptr);

  account_leave(*m_current);
  m_current = nullptr;
}

bool EevdfQueue::charge(std::uint64_t delta_ns) noexcept {
  if (m_current == nullptr) {
    return false;
  }

  Entity& entity = *m_current;
  const std::uint64_t delta = entity.to_virtual(delta_ns);
  entity.m_vruntime += delta;
  m_weighted_vruntime += static_cast<unsigned __int128>(entity.m_weight) * delta;

  if (entity.m_vruntime < entity.m_vdeadline) {
    return false;
  }

  entity.m_vdeadline = entity.m_vruntime + entity.to_virtual(entity.m_slice_ns);
  return true;
}

void EevdfQueue::yield_current() const noexcept {
  if (m_current != nullptr) {
    m_current->m_vdeadline += m_current->to_virtual(m_current->m_slice_ns);
  }
}

// With V fixed, keeping w * (V - v) is keeping what the entity is owed, and the new virtual runtime
// comes out as V - (V - v) * w / w'. Put that into the average and V does not move:
//
//   sum(w * v) + w' * v' - w * v = sum(w * v) + (w' - w) * V   and   W + w' - w
//
// are V * W and W with the same (w' - w) * V and (w' - w) added. Nobody on the queue changes from
// eligible to not or the other way around, up to the rounding of V itself.
void EevdfQueue::reweight(Entity& entity, const std::uint32_t weight) noexcept {
  KASSERT(entity.is_queued() || &entity == m_current);

  const bool queued = entity.is_queued();
  if (queued) {
    m_tree.erase(entity);
  }

  const std::uint64_t now = virtual_time();
  const std::uint32_t old_weight = entity.m_weight;

  m_weighted_vruntime -= static_cast<unsigned __int128>(old_weight) * entity.m_vruntime;
  m_total_weight -= old_weight;

  const auto old_lag = static_cast<__int128>(now) - entity.m_vruntime;
  const auto old_left = static_cast<__int128>(entity.m_vdeadline) - entity.m_vruntime;

  entity.set_weight(weight);

  const __int128 lag = old_lag * old_weight / entity.m_weight;
  const __int128 left = old_left * old_weight / entity.m_weight;
  const __int128 vruntime = static_cast<__int128>(now) - lag;

  entity.m_vruntime = vruntime > 0 ? static_cast<std::uint64_t>(vruntime) : 0;
  entity.m_vdeadline = static_cast<std::uint64_t>(entity.m_vruntime + left);

  account_join(entity);
  if (queued) {
    m_tree.insert(entity);
  }
}

Entity* EevdfQueue::pick() const noexcept {
  Entity* best = nullptr;

  // The leftmost eligible node is the one wanted. A left subtree either has an eligible node, in
  // which case that beats this one and everything to the right, or it can be skipped altogether.
  for (Entity* node = m_tree.root(); node != nullptr;) {
    const Entity* left = Tree::left(*node);
    if (left != nullptr && is_eligible_at(left->m_subtree_min_vruntime)) {
      node = Tree::left(*node);
    } else if (is_eligible(*node)) {
      best = node;
      break;
    } else {
      node = Tree::right(*node);
    }
  }

  if (m_current == nullptr) {
    KASSERT(best != nullptr || m_tree.empty());
    return best;
  }

  // Ties go to the current one: switching for no reason is never free.
  if (best == nullptr || (is_eligible(*m_current) && m_current->m_vdeadline <= best->m_vdeadline)) {
    return m_current;
  }

  return best;
}

bool EevdfQueue::preempts_current(const Entity& entity) const noexcept {
  return m_current != nullptr && is_eligible(entity) && entity.m_vdeadline < m_current->m_vdeadline;
}

std::uint64_t EevdfQueue::slice_left_ns() const noexcept {
  if (m_current == nullptr) {
    return 0;
  }

  const Entity& entity = *m_current;
  if (entity.m_vruntime >= entity.m_vdeadline) {
    return 0;
  }

  // A yield can push a deadline further than one slice away.
  return std::min(entity.to_real(entity.m_vdeadline - entity.m_vruntime), entity.m_slice_ns);
}

void EevdfQueue::TreeTraits::update(Entity& entity) noexcept {
  std::uint64_t lowest = entity.m_vruntime;

  for (const Entity* child : {Tree::left(entity), Tree::right(entity)}) {
    if (child != nullptr) {
      lowest = std::min(lowest, child->m_subtree_min_vruntime);
    }
  }

  entity.m_subtree_min_vruntime = lowest;
}

bool EevdfQueue::is_consistent() const noexcept {
  if (!m_tree.is_consistent()) {
    return false;
  }

  std::uint32_t count = 0;
  unsigned __int128 weighted = 0;
  std::uint64_t weight = 0;

  for (const Entity* entity = m_tree.first(); entity != nullptr; entity = Tree::next(*entity)) {
    std::uint64_t lowest = entity->m_vruntime;
    for (const Entity* child : {Tree::left(*entity), Tree::right(*entity)}) {
      if (child != nullptr) {
        lowest = std::min(lowest, child->m_subtree_min_vruntime);
      }
    }

    if (entity->m_subtree_min_vruntime != lowest) {
      return false;
    }

    ++count;
    weighted += static_cast<unsigned __int128>(entity->m_weight) * entity->m_vruntime;
    weight += entity->m_weight;
  }

  if (m_current != nullptr) {
    weighted += static_cast<unsigned __int128>(m_current->m_weight) * m_current->m_vruntime;
    weight += m_current->m_weight;
  }

  return count == m_queued && weighted == m_weighted_vruntime && weight == m_total_weight;
}
}  // namespace kernel::sched
