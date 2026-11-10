/// @file cspace.hpp
/// @brief Capability spaces: a table of capabilities, and everything that is done to them.
///
/// A CSpace is a two-level table, 256 nodes of 256 slots, so 65536 capabilities at most, with the
/// nodes allocated as the table fills up. A CapId names a slot by its index and the generation of
/// the slot, which moves on every time it is emptied, so a stale ID finds nothing.
///
/// Lookups take no lock. They run in an RCU read section, and look at a slot the way a seqlock
/// reader would: the header, then the rest, then the header again, which a change in between
/// would have changed. Everything that changes a slot holds the CSpace's lock. Nodes are only ever
/// added, until the CSpace itself goes, a grace period after its last reference.
///
/// A CSpace is a capability object itself, so one can be handed out and shared like any other.
/// Like any other, a cycle of references, a CSpace holding a capability to something that holds
/// the CSpace, keeps everything in it alive; clear() is what breaks one.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>

#include "core/cap.hpp"
#include "core/errors.hpp"
#include "utils/lock.hpp"
#include "utils/rcu/rcu.hpp"
#include "utils/rcu/rcu_ptr.hpp"

namespace kernel::core::ipc {
/// @brief A slot: the header, and the badge.
struct alignas(16) CapSlot {
  std::atomic<std::uint64_t> header{0};
  std::atomic<std::uint64_t> badge{0};
};

static_assert(sizeof(CapSlot) == 16);

/// @brief The second level of a CSpace: 256 slots, with what goes with each.
struct alignas(std::hardware_destructive_interference_size) CapTableNode {
  static constexpr std::uint32_t SLOTS = 256;
  static constexpr std::uint32_t WORDS = SLOTS / 64;

  std::array<CapSlot, SLOTS> slots{};
  std::array<std::atomic<std::uint32_t>, SLOTS> epochs{};  ///< See CapObject::epoch().
  std::array<std::atomic<std::uint16_t>, SLOTS> generations{};
  std::array<std::uint64_t, WORDS> used{};  ///< Under the CSpace's lock. Taken slots are set.
};

/// @brief A table of capabilities. See the file comment.
class alignas(std::hardware_destructive_interference_size) CSpace final
    : public TypedCapObject<CSpace, CapType::CSpace> {
 public:
  static constexpr std::uint32_t NODE_BITS = 8;
  static constexpr std::uint32_t SLOTS_PER_NODE = CapTableNode::SLOTS;
  static constexpr std::uint32_t MAX_NODES = 256;
  static constexpr std::uint32_t MAX_CAPACITY = MAX_NODES * SLOTS_PER_NODE;

  constexpr CSpace() noexcept = default;

  /// @brief Lets go of every capability left. A grace period after the last reference, in a
  ///        kernel thread, like every object's end.
  ~CSpace() noexcept;

  /// @brief Sets up the cache the tables come from. Part of ipc::initialize().
  static void initialize_cache() noexcept;

  /// @brief How many capabilities there are in it.
  [[nodiscard]] std::uint32_t active_count() const noexcept {
    return m_active.load(std::memory_order_relaxed);
  }

  /// @brief Finds the capability @p id names, and checks it has @p required. The object stays for
  ///        as long as @p rcu does.
  /// @return NotFound for an empty, stale or revoked capability, InvalidArgument for an ID that
  ///         cannot be one, AccessDenied for one without @p required.
  [[nodiscard, gnu::always_inline]] std::expected<CapEntry, Error> lookup(
      const CapId id, [[maybe_unused]] const utils::rcu::ReadGuard& rcu,
      const CapRights required = CapRights::None) const noexcept {
    const std::uint32_t index = id.get_index();
    const std::uint16_t generation = id.get_generation();
    if (index == 0 || index >= MAX_CAPACITY || generation == 0) [[unlikely]] {
      return std::unexpected(Error::InvalidArgument);
    }

    const CapTableNode* node = m_nodes[index >> NODE_BITS].load(rcu);
    if (node == nullptr) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    const std::uint32_t i = index & (SLOTS_PER_NODE - 1);
    const CapSlot& slot = node->slots[i];
    const std::uint64_t raw = slot.header.load(std::memory_order_acquire);
    const CapHeader header{raw};
    if (header.is_null() || header.get_gen_low() != (generation & 0xFu) ||
        node->generations[i].load(std::memory_order_relaxed) != generation) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    const std::uint64_t badge = slot.badge.load(std::memory_order_relaxed);
    const std::uint32_t epoch = node->epochs[i].load(std::memory_order_relaxed);

    // Whatever was read in between belongs to this header, unless the header changed meanwhile.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.header.load(std::memory_order_relaxed) != raw) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    if (!has_cap_flag(header.get_flags(), CapFlags::Original) && epoch != header.object()->epoch())
        [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    if (!has_rights(header.get_rights(), required)) [[unlikely]] {
      return std::unexpected(Error::AccessDenied);
    }

    return CapEntry{.header = header, .badge = badge};
  }

  /// @brief Finds the capability @p id names, which must be to a @p T and have @p required, and
  ///        takes a reference on the object, for use outside a read section.
  template <typename T>
  [[nodiscard]] std::expected<Held<T>, Error> acquire(
      const CapId id, const CapRights required = CapRights::None) const noexcept {
    const utils::rcu::ReadGuard rcu;
    const auto entry = lookup(id, rcu, required);
    if (!entry) [[unlikely]] {
      return std::unexpected(entry.error());
    }

    if (entry->type() != T::TYPE) [[unlikely]] {
      return std::unexpected(Error::InvalidArgument);
    }

    T* object = entry->as<T>();
    if (!object->try_add_ref()) [[unlikely]] {
      return std::unexpected(Error::NotFound);
    }

    return Held<T>{.object = CapRef<T>::adopt(object),
                   .rights = entry->rights(),
                   .flags = entry->flags(),
                   .badge = entry->badge};
  }

  /// @brief Puts an original capability to @p object in a free slot. The slot takes a reference of
  ///        its own; the caller keeps whatever it had.
  /// @return The new ID; OutOfMemory if the table is full or no node can be had, InvalidState once
  ///         the CSpace is cleared, InvalidArgument for an object that cannot be named.
  [[nodiscard]] std::expected<CapId, Error> insert(CapObject& object, CapRights rights,
                                                   CapFlags flags = CapFlags::Original,
                                                   std::uint64_t badge = 0) noexcept;

  /// @brief Puts a capability derived from @p src in @p dest, with @p rights, which the source
  ///        must have all of, and @p badge, if not 0, which takes Mint and a source that has no
  ///        badge yet. @p extra may add Pinned.
  ///
  /// One-shot capabilities cannot be copied, only moved.
  /// @return PrivilegeEscalationDetected for rights the source does not have; AccessDenied or
  ///         SecurityViolation for a badge that may not be set; InvalidArgument for a one-shot one.
  [[nodiscard]] std::expected<CapId, Error> mint(CapId src, CSpace& dest, CapRights rights,
                                                 std::uint64_t badge = 0,
                                                 CapFlags extra = CapFlags::None) const noexcept;

  /// @brief mint() with the source's rights less those not in @p mask, and its badge.
  [[nodiscard]] std::expected<CapId, Error> copy(CapId src, CSpace& dest,
                                                 CapRights mask = CapRights::All) const noexcept;

  /// @brief Moves the capability from @p src here to @p dest, as it is: rights, flags, badge, and
  ///        whether it is the original. If it fails, the capability stays where it was.
  /// @return AccessDenied for a pinned one.
  [[nodiscard]] std::expected<CapId, Error> move(CapId src, CSpace& dest) noexcept;

  /// @brief Empties the slot @p id names. @return AccessDenied for a pinned capability.
  [[nodiscard]] std::expected<void, Error> remove(CapId id) noexcept;

  /// @brief Revokes everything derived from the original capability @p id, in every CSpace: they
  ///        stop working at once, and those in this one are removed. Takes Revoke.
  /// @return How many were removed from this CSpace.
  [[nodiscard]] std::expected<std::uint32_t, Error> revoke(CapId id) noexcept;

  /// @brief Takes the one-shot capability @p id out, handing its reference to the caller.
  template <typename T>
  [[nodiscard]] std::expected<Held<T>, Error> consume(
      const CapId id, const CapRights required = CapRights::None) noexcept {
    const auto taken = take_one_shot(id, T::TYPE, required);
    if (!taken) [[unlikely]] {
      return std::unexpected(taken.error());
    }

    return Held<T>{.object = CapRef<T>::adopt(static_cast<T*>(taken->object())),
                   .rights = taken->rights(),
                   .flags = taken->flags(),
                   .badge = taken->badge};
  }

  /// @brief Empties every slot, and refuses new capabilities from now on. What breaks a cycle of
  ///        references through this CSpace; the destructor does it too.
  void clear() noexcept;

 private:
  // A slot taken under the lock but not filled in yet.
  struct Reservation {
    CapTableNode* node;
    std::uint32_t slot;
    std::uint16_t generation;
    std::uint32_t index;
  };

  // What a slot held, once emptied: the slot's reference and capability go to whoever emptied it.
  struct Taken {
    CapHeader header;
    std::uint64_t badge;
    std::uint32_t epoch;
  };

  [[nodiscard]] std::expected<Reservation, Error> reserve_locked() noexcept;
  void unreserve_locked(const Reservation& reservation) noexcept;
  void publish_locked(const Reservation& reservation, CapType type, CapRights rights,
                      CapFlags flags, std::uint64_t badge, std::uint32_t epoch,
                      const CapObject* object) noexcept;
  [[nodiscard]] std::expected<Taken, Error> take_locked(CapId id) noexcept;
  void empty_slot_locked(CapTableNode& node, std::uint32_t slot) noexcept;

  [[nodiscard]] std::expected<CapEntry, Error> take_one_shot(CapId id, CapType type,
                                                             CapRights required) noexcept;

  // The table grows by whole nodes, from node 0 up. A node with room has its bit set here.
  mutable utils::QSpinlock m_lock{};
  bool m_cleared{false};
  std::uint32_t m_node_count{0};
  std::array<std::uint64_t, MAX_NODES / 64> m_room{};
  std::atomic<std::uint32_t> m_active{0};

  std::array<utils::rcu::Ptr<CapTableNode>, MAX_NODES> m_nodes{};
};
}  // namespace kernel::core::ipc
