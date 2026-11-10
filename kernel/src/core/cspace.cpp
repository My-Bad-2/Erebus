#include "core/cspace.hpp"

#include <bit>
#include <memory>

#include "memory/heap.hpp"
#include "utils/assert.hpp"
#include "utils/logger.hpp"

namespace kernel::core::ipc {
namespace {
memory::heap::KmemCache* g_node_cache = nullptr;

// Generations run from 1 to MAX_GEN: 0 is null CapId.
[[nodiscard]] constexpr std::uint16_t next_generation(const std::uint16_t current) noexcept {
  const auto next = static_cast<std::uint16_t>((current + 1) & CapId::MAX_GEN);
  return next == 0 ? 1 : next;
}

[[nodiscard]] CapTableNode* allocate_node() noexcept {
  void* memory = nullptr;
  if (g_node_cache != nullptr) [[likely]] {
    if (const auto got = g_node_cache->alloc()) {
      memory = *got;
    }
  } else {
    memory = memory::heap::kmalloc(sizeof(CapTableNode), alignof(CapTableNode));
  }

  return memory != nullptr ? std::construct_at(static_cast<CapTableNode*>(memory)) : nullptr;
}

using Guard = utils::IrqSaveGuard<utils::QSpinlock>;
}  // namespace

void CSpace::initialize_cache() noexcept {
  if (g_node_cache != nullptr) {
    return;
  }

  const auto cache = memory::heap::KmemCache::create(sizeof(CapTableNode), alignof(CapTableNode));
  if (!cache) {
    utils::logger::fatal("Capabilities: no cache for CSpace tables: {:#x}\n",
                         std::to_underlying(cache.error()));
  }

  g_node_cache = *cache;
}

CSpace::~CSpace() noexcept {
  clear();

  for (std::uint32_t n = 0; n < m_node_count; ++n) {
    memory::heap::kfree(m_nodes[n].exchange(nullptr));
  }
}

std::expected<CSpace::Reservation, Error> CSpace::reserve_locked() noexcept {
  if (m_cleared) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  std::uint32_t n = MAX_NODES;
  for (std::uint32_t w = 0; w < m_room.size(); ++w) {
    if (m_room[w] != 0) {
      n = w * 64 + static_cast<std::uint32_t>(std::countr_zero(m_room[w]));
      break;
    }
  }

  if (n == MAX_NODES) {
    if (m_node_count == MAX_NODES) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    CapTableNode* node = allocate_node();
    if (node == nullptr) [[unlikely]] {
      return std::unexpected(Error::OutOfMemory);
    }

    n = m_node_count++;
    if (n == 0) {
      node->used[0] = 1;
    }

    m_nodes[n].store(node);
    m_room[n / 64] |= std::uint64_t{1} << (n % 64);
  }

  CapTableNode& node = *m_nodes[n].load_writer_locked();
  std::uint32_t slot = SLOTS_PER_NODE;
  for (std::uint32_t w = 0; w < CapTableNode::WORDS; ++w) {
    if (const std::uint64_t free = ~node.used[w]; free != 0) {
      slot = w * 64 + static_cast<std::uint32_t>(std::countr_zero(free));
      node.used[w] |= std::uint64_t{1} << (slot % 64);
      break;
    }
  }

  KASSERT(slot < SLOTS_PER_NODE);  // it was on m_room
  bool full = true;
  for (const std::uint64_t word : node.used) {
    full &= word == ~std::uint64_t{0};
  }

  if (full) {
    m_room[n / 64] &= ~(std::uint64_t{1} << (n % 64));
  }

  std::uint16_t generation = node.generations[slot].load(std::memory_order_relaxed);
  if (generation == 0) {
    generation = 1;
    node.generations[slot].store(1, std::memory_order_relaxed);
  }

  return Reservation{
      .node = &node, .slot = slot, .generation = generation, .index = (n << NODE_BITS) | slot};
}

void CSpace::unreserve_locked(const Reservation& reservation) noexcept {
  const std::uint32_t n = reservation.index >> NODE_BITS;
  reservation.node->used[reservation.slot / 64] &= ~(std::uint64_t{1} << (reservation.slot % 64));
  m_room[n / 64] |= std::uint64_t{1} << (n % 64);
}

void CSpace::publish_locked(const Reservation& reservation, const CapType type,
                            const CapRights rights, const CapFlags flags, const std::uint64_t badge,
                            const std::uint32_t epoch, const CapObject* const object) noexcept {
  CapSlot& slot = reservation.node->slots[reservation.slot];
  slot.badge.store(badge, std::memory_order_relaxed);
  reservation.node->epochs[reservation.slot].store(epoch, std::memory_order_relaxed);

  slot.header.store(CapHeader::make(type, rights, flags, reservation.generation, object).raw(),
                    std::memory_order_release);
  m_active.fetch_add(1, std::memory_order_relaxed);
}

void CSpace::empty_slot_locked(CapTableNode& node, const std::uint32_t slot) noexcept {
  node.slots[slot].header.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  node.slots[slot].badge.store(0, std::memory_order_relaxed);
  node.generations[slot].store(
      next_generation(node.generations[slot].load(std::memory_order_relaxed)),
      std::memory_order_relaxed);

  node.used[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
  m_active.fetch_sub(1, std::memory_order_relaxed);
}

std::expected<CSpace::Taken, Error> CSpace::take_locked(const CapId id) noexcept {
  const std::uint32_t index = id.get_index();
  const std::uint16_t generation = id.get_generation();
  if (index == 0 || index >= MAX_CAPACITY || generation == 0) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const std::uint32_t n = index >> NODE_BITS;
  CapTableNode* node = m_nodes[n].load_writer_locked();
  const std::uint32_t slot = index & (SLOTS_PER_NODE - 1);
  if (node == nullptr || node->generations[slot].load(std::memory_order_relaxed) != generation)
      [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  const CapHeader header{node->slots[slot].header.load(std::memory_order_relaxed)};
  if (header.is_null()) [[unlikely]] {
    return std::unexpected(Error::NotFound);
  }

  const Taken taken{
      .header = header,
      .badge = node->slots[slot].badge.load(std::memory_order_relaxed),
      .epoch = node->epochs[slot].load(std::memory_order_relaxed),
  };
  empty_slot_locked(*node, slot);
  m_room[n / 64] |= std::uint64_t{1} << (n % 64);
  return taken;
}

std::expected<CapId, Error> CSpace::insert(CapObject& object, const CapRights rights,
                                           const CapFlags flags,
                                           const std::uint64_t badge) noexcept {
  if (object.cap_type() == CapType::Null || !CapHeader::is_representable(&object)) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  const CapFlags final_flags = badge != 0 ? flags | CapFlags::Badged : flags;
  Guard guard{m_lock};
  const auto reservation = reserve_locked();
  if (!reservation) [[unlikely]] {
    return std::unexpected(reservation.error());
  }

  object.add_ref();
  object.add_cap();
  publish_locked(*reservation, object.cap_type(), rights, final_flags, badge, object.epoch(),
                 &object);
  return CapId::make(reservation->index, reservation->generation);
}

std::expected<CapId, Error> CSpace::mint(const CapId src, CSpace& dest, const CapRights rights,
                                         const std::uint64_t badge,
                                         const CapFlags extra) const noexcept {
  CapEntry source;
  std::uint32_t epoch;
  {
    const utils::rcu::ReadGuard rcu;
    const auto entry = lookup(src, rcu);
    if (!entry) [[unlikely]] {
      return std::unexpected(entry.error());
    }

    source = *entry;
    if (has_cap_flag(source.flags(), CapFlags::OneShot)) [[unlikely]] {
      return std::unexpected(Error::InvalidArgument);
    }

    if (!has_rights(source.rights(), rights)) [[unlikely]] {
      return std::unexpected(Error::PrivilegeEscalationDetected);
    }

    if (badge != 0 && badge != source.badge) {
      if (!has_rights(source.rights(), CapRights::Mint)) [[unlikely]] {
        return std::unexpected(Error::AccessDenied);
      }

      if (has_cap_flag(source.flags(), CapFlags::Badged)) [[unlikely]] {
        return std::unexpected(Error::SecurityViolation);
      }
    }

    // The epoch the source is valid in. A revoke after this makes the new capability dead on
    // arrival.
    epoch = source.object()->epoch();
    if (!source.object()->try_add_ref()) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }
  }

  CapObject* object = source.object();
  const std::uint64_t final_badge = badge != 0 ? badge : source.badge;
  CapFlags flags = (source.flags() & ~CapFlags::Original) | (extra & CapFlags::Pinned);
  if (final_badge != 0) {
    flags = flags | CapFlags::Badged;
  }

  {
    Guard guard{dest.m_lock};
    if (const auto reservation = dest.reserve_locked()) [[likely]] {
      object->add_cap();
      dest.publish_locked(*reservation, source.type(), rights, flags, final_badge, epoch, object);
      return CapId::make(reservation->index, reservation->generation);
    } else {
      object->release_ref();
      return std::unexpected(reservation.error());
    }
  }
}

std::expected<CapId, Error> CSpace::copy(const CapId src, CSpace& dest,
                                         const CapRights mask) const noexcept {
  CapRights rights;
  {
    const utils::rcu::ReadGuard rcu;
    const auto entry = lookup(src, rcu);
    if (!entry) [[unlikely]] {
      return std::unexpected(entry.error());
    }

    rights = entry->rights() & mask;
  }

  return mint(src, dest, rights);
}

std::expected<CapId, Error> CSpace::move(const CapId src, CSpace& dest) noexcept {
  Reservation reservation;
  {
    Guard guard{dest.m_lock};
    const auto reserved = dest.reserve_locked();
    if (!reserved) [[unlikely]] {
      return std::unexpected(reserved.error());
    }

    reservation = *reserved;
  }

  std::expected<Taken, Error> taken;
  {
    Guard guard{m_lock};
    const std::uint32_t index = src.get_index();
    const CapTableNode* node = index != 0 && index < MAX_CAPACITY
                                   ? m_nodes[index >> NODE_BITS].load_writer_locked()
                                   : nullptr;
    const CapHeader header{node != nullptr ? node->slots[index & (SLOTS_PER_NODE - 1)].header.load(
                                                 std::memory_order_relaxed)
                                           : 0};
    if (has_cap_flag(header.get_flags(), CapFlags::Pinned)) [[unlikely]] {
      taken = std::unexpected(Error::AccessDenied);
    } else {
      taken = take_locked(src);
    }
  }

  CapObject* object = taken ? taken->header.object() : nullptr;
  {
    Guard guard{dest.m_lock};
    if (taken && !dest.m_cleared) [[likely]] {
      dest.publish_locked(reservation, taken->header.get_type(), taken->header.get_rights(),
                          taken->header.get_flags(), taken->badge, taken->epoch, object);
      return CapId::make(reservation.index, reservation.generation);
    }

    dest.unreserve_locked(reservation);
  }

  if (!taken) [[unlikely]] {
    return std::unexpected(taken.error());
  }

  object->drop_cap();
  object->release_ref();
  return std::unexpected(Error::InvalidState);
}

std::expected<void, Error> CSpace::remove(const CapId id) noexcept {
  std::expected<Taken, Error> taken;
  {
    Guard guard{m_lock};
    const std::uint32_t index = id.get_index();
    const CapTableNode* node = index != 0 && index < MAX_CAPACITY
                                   ? m_nodes[index >> NODE_BITS].load_writer_locked()
                                   : nullptr;
    if (node != nullptr &&
        has_cap_flag(CapHeader{node->slots[index & (SLOTS_PER_NODE - 1)].header.load(
                                   std::memory_order_relaxed)}
                         .get_flags(),
                     CapFlags::Pinned)) [[unlikely]] {
      return std::unexpected(Error::AccessDenied);
    }

    taken = take_locked(id);
  }

  if (!taken) [[unlikely]] {
    return std::unexpected(taken.error());
  }

  CapObject* object = taken->header.object();
  object->drop_cap();
  object->release_ref();
  return {};
}

std::expected<CapEntry, Error> CSpace::take_one_shot(const CapId id, const CapType type,
                                                     const CapRights required) noexcept {
  std::expected<Taken, Error> taken;
  {
    Guard guard{m_lock};
    const std::uint32_t index = id.get_index();
    const CapTableNode* node = index != 0 && index < MAX_CAPACITY
                                   ? m_nodes[index >> NODE_BITS].load_writer_locked()
                                   : nullptr;
    const std::uint32_t slot = index & (SLOTS_PER_NODE - 1);
    if (node == nullptr ||
        node->generations[slot].load(std::memory_order_relaxed) != id.get_generation()) {
      return std::unexpected(Error::NotFound);
    }

    const CapHeader header{node->slots[slot].header.load(std::memory_order_relaxed)};
    if (header.is_null()) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    if (header.get_type() != type || !has_cap_flag(header.get_flags(), CapFlags::OneShot))
        [[unlikely]] {
      return std::unexpected(Error::InvalidArgument);
    }

    if (!has_rights(header.get_rights(), required)) [[unlikely]] {
      return std::unexpected(Error::AccessDenied);
    }

    taken = take_locked(id);
  }

  if (!taken) [[unlikely]] {
    return std::unexpected(taken.error());
  }

  taken->header.object()->drop_cap();
  return CapEntry{.header = taken->header, .badge = taken->badge};
}

std::expected<std::uint32_t, Error> CSpace::revoke(const CapId id) noexcept {
  CapObject* object;
  {
    const utils::rcu::ReadGuard rcu;
    const auto entry = lookup(id, rcu, CapRights::Revoke);
    if (!entry) [[unlikely]] {
      return std::unexpected(entry.error());
    }

    if (!has_cap_flag(entry->flags(), CapFlags::Original)) [[unlikely]] {
      return std::unexpected(Error::AccessDenied);
    }

    object = entry->object();
    (void)object->bump_epoch();
  }

  std::uint32_t removed = 0;
  Guard guard{m_lock};
  for (std::uint32_t n = 0; n < m_node_count; ++n) {
    CapTableNode& node = *m_nodes[n].load_writer_locked();
    for (std::uint32_t slot = 0; slot < SLOTS_PER_NODE; ++slot) {
      const CapHeader header{node.slots[slot].header.load(std::memory_order_relaxed)};
      if (header.is_null() || header.object() != object ||
          has_cap_flag(header.get_flags(), CapFlags::Original)) {
        continue;
      }

      empty_slot_locked(node, slot);
      m_room[n / 64] |= std::uint64_t{1} << (n % 64);
      object->drop_cap();
      object->release_ref();
      ++removed;
    }
  }

  return removed;
}

void CSpace::clear() noexcept {
  constexpr std::uint32_t BATCH = 32;
  std::array<CapObject*, BATCH> batch{};

  std::uint32_t n = 0;
  std::uint32_t slot = 0;
  while (true) {
    std::uint32_t count = 0;
    {
      Guard guard{m_lock};
      m_cleared = true;
      for (; n < m_node_count && count < BATCH; slot = 0, ++n) {
        CapTableNode& node = *m_nodes[n].load_writer_locked();
        for (; slot < SLOTS_PER_NODE && count < BATCH; ++slot) {
          const CapHeader header{node.slots[slot].header.load(std::memory_order_relaxed)};
          if (!header.is_null()) {
            batch[count++] = header.object();
            empty_slot_locked(node, slot);
          }
        }

        if (slot < SLOTS_PER_NODE) {
          break;
        }
      }
    }

    if (count == 0) {
      return;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
      batch[i]->drop_cap();
      batch[i]->release_ref();
    }
  }
}
}  // namespace kernel::core::ipc
