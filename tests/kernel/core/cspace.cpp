// CSpaces: what a capability names and allows, how it is copied, moved and revoked, when its
// object goes, and lookups that run while everything changes underneath them.
#include "core/cspace.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "../sched/support.hpp"
#include "drivers/clock.hpp"
#include "ktest.hpp"
#include "memory/quota.hpp"
#include "probe.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::testing {
namespace {
namespace ipc = core::ipc;
using core_probe::Counters;
using core_probe::Probe;
using ipc::CapFlags;
using ipc::CapId;
using ipc::CapRef;
using ipc::CapRights;
using ipc::CSpace;
using klib::Duration;

[[nodiscard]] CapRef<CSpace> new_cspace() noexcept {
  auto made = CSpace::create();
  KEXPECT(made.has_value());
  return made ? std::move(*made) : CapRef<CSpace>{};
}

[[nodiscard]] CapRef<Probe> new_probe(Counters& counters) noexcept {
  auto made = Probe::create(counters);
  KEXPECT(made.has_value());
  return made ? std::move(*made) : CapRef<Probe>{};
}

// What a lookup says about @p id, in a read section of its own.
[[nodiscard]] std::expected<ipc::CapEntry, Error> look(const CSpace& cs, const CapId id,
                                                       const CapRights required = CapRights::None) {
  const utils::rcu::ReadGuard rcu;
  return cs.lookup(id, rcu, required);
}

[[nodiscard]] bool found(const CSpace& cs, const CapId id) { return look(cs, id).has_value(); }
}  // namespace

KTEST(CSpace, a_capability_is_found_until_it_is_removed_and_never_again) {
  Counters counters;
  auto cs = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(cs && probe);

  const auto id = cs->insert(*probe, CapRights::ReadWrite);
  KREQUIRE(id.has_value());
  KEXPECT_EQ(probe->cap_count(), 1u);
  KEXPECT_EQ(probe->ref_count(), 2u);
  KEXPECT_EQ(cs->active_count(), 1u);

  const auto entry = look(*cs, *id);
  KREQUIRE(entry.has_value());
  KEXPECT(entry->object() == probe.get());
  KEXPECT(entry->rights() == CapRights::ReadWrite);
  KEXPECT(ipc::has_cap_flag(entry->flags(), CapFlags::Original));
  KEXPECT(look(*cs, *id, CapRights::Grant).error() == Error::AccessDenied);

  KEXPECT(cs->remove(*id).has_value());
  KEXPECT(look(*cs, *id).error() == Error::NotFound);
  KEXPECT(cs->remove(*id).error() == Error::NotFound);
  KEXPECT_EQ(cs->active_count(), 0u);

  // The slot comes back, with a new generation: the old ID names nothing.
  const auto again = cs->insert(*probe, CapRights::Read);
  KREQUIRE(again.has_value());
  KEXPECT_EQ(again->get_index(), id->get_index());
  KEXPECT(again->get_generation() != id->get_generation());
  KEXPECT(!found(*cs, *id));
  KEXPECT(found(*cs, *again));

  KEXPECT(look(*cs, CapId{}).error() == Error::InvalidArgument);
  KEXPECT(look(*cs, CapId::make(CSpace::MAX_CAPACITY - 1, 1)).error() == Error::NotFound);

  cs.reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
  KEXPECT_EQ(counters.unreachable.load(), 2u);  // once for each time its last capability went
}

KTEST(CSpace, rights_only_shrink_and_a_badge_is_set_once) {
  Counters counters;
  auto a = new_cspace();
  auto b = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(a && b && probe);

  const auto all = a->insert(*probe, CapRights::All);
  KREQUIRE(all.has_value());

  const auto badged = a->mint(*all, *b, CapRights::ReadWrite, 7);
  KREQUIRE(badged.has_value());
  const auto entry = look(*b, *badged);
  KREQUIRE(entry.has_value());
  KEXPECT_EQ(entry->badge, 7u);
  KEXPECT(ipc::has_cap_flag(entry->flags(), CapFlags::Badged));
  KEXPECT(!ipc::has_cap_flag(entry->flags(), CapFlags::Original));

  // More rights than it has, or another badge on one that has one, never.
  KEXPECT(b->mint(*badged, *a, CapRights::All).error() == Error::PrivilegeEscalationDetected);
  KEXPECT(b->mint(*badged, *a, CapRights::ReadWrite, 9).error() == Error::AccessDenied);

  const auto copied = b->copy(*badged, *a, CapRights::Read);
  KREQUIRE(copied.has_value());
  KEXPECT(look(*a, *copied)->rights() == CapRights::Read);
  KEXPECT_EQ(look(*a, *copied)->badge, 7u);

  // A badge takes Mint, and a capability that has Mint but already carries a badge cannot change
  // it.
  const auto plain = a->insert(*probe, CapRights::ReadWrite);
  KREQUIRE(plain.has_value());
  KEXPECT(a->mint(*plain, *b, CapRights::Read, 5).error() == Error::AccessDenied);

  const auto minted_badged = a->mint(*all, *a, CapRights::All, 3);
  KREQUIRE(minted_badged.has_value());
  KEXPECT(a->mint(*minted_badged, *b, CapRights::Read, 4).error() == Error::SecurityViolation);

  a.reset();
  b.reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
}

KTEST(CSpace, a_move_keeps_everything_and_pinned_or_one_shot_ones_stay_put) {
  Counters counters;
  auto a = new_cspace();
  auto b = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(a && b && probe);

  const auto original = a->insert(*probe, CapRights::All, CapFlags::Original, 11);
  KREQUIRE(original.has_value());
  const auto moved = a->move(*original, *b);
  KREQUIRE(moved.has_value());
  KEXPECT(!found(*a, *original));

  const auto entry = look(*b, *moved);
  KREQUIRE(entry.has_value());
  KEXPECT(entry->rights() == CapRights::All);
  KEXPECT_EQ(entry->badge, 11u);
  KEXPECT(ipc::has_cap_flag(entry->flags(), CapFlags::Original));
  KEXPECT_EQ(probe->cap_count(), 1u);

  // An original that moved can still revoke.
  KEXPECT(b->revoke(*moved).has_value());

  const auto pinned = a->insert(*probe, CapRights::Read, CapFlags::Original | CapFlags::Pinned);
  KREQUIRE(pinned.has_value());
  KEXPECT(a->move(*pinned, *b).error() == Error::AccessDenied);
  KEXPECT(a->remove(*pinned).error() == Error::AccessDenied);
  KEXPECT(found(*a, *pinned));

  const auto once = a->insert(*probe, CapRights::Write, CapFlags::OneShot);
  KREQUIRE(once.has_value());
  KEXPECT(a->copy(*once, *b).error() == Error::InvalidArgument);
  KEXPECT(a->consume<Probe>(*pinned).error() == Error::InvalidArgument);

  const std::uint32_t refs = probe->ref_count();
  auto taken = a->consume<Probe>(*once, CapRights::Write);
  KREQUIRE(taken.has_value());
  KEXPECT(taken->object.get() == probe.get());
  KEXPECT_EQ(probe->ref_count(), refs);  // the slot's reference went to the caller
  KEXPECT(a->consume<Probe>(*once).error() == Error::NotFound);
  taken->object.reset();

  // Into a CSpace that was cleared, nothing goes, and the capability stays where it was.
  const auto stays = a->insert(*probe, CapRights::Read);
  KREQUIRE(stays.has_value());
  const std::uint32_t caps = probe->cap_count();
  b->clear();
  KEXPECT_EQ(probe->cap_count(), caps - 1);  // b's original went with it
  KEXPECT(a->move(*stays, *b).error() == Error::InvalidState);
  KEXPECT(found(*a, *stays));
  KEXPECT(b->insert(*probe, CapRights::Read).error() == Error::InvalidState);

  a.reset();
  b.reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
}

// Revoking the original kills every derived capability, wherever it went and however many hands
// it went through, at once. The original stays, and can hand out new ones.
KTEST(CSpace, a_revoke_reaches_derived_capabilities_in_every_cspace) {
  Counters counters;
  auto a = new_cspace();
  auto b = new_cspace();
  auto c = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(a && b && c && probe);

  const auto original = a->insert(*probe, CapRights::All);
  KREQUIRE(original.has_value());
  const auto child = a->mint(*original, *b, CapRights::ReadWrite, 1);
  const auto grandchild = b->copy(*child, *c, CapRights::Read);
  const auto local = a->copy(*original, *a, CapRights::Write);
  KREQUIRE(child && grandchild && local);

  const auto removed = a->revoke(*original);
  KREQUIRE(removed.has_value());
  KEXPECT_EQ(*removed, 1u);  // the copy in a
  KEXPECT(!found(*a, *local));
  KEXPECT(!found(*b, *child));
  KEXPECT(!found(*c, *grandchild));
  KEXPECT(found(*a, *original));

  // Dead copies cannot make live ones.
  KEXPECT(b->copy(*child, *c).error() == Error::NotFound);

  const auto fresh = a->mint(*original, *b, CapRights::Read);
  KREQUIRE(fresh.has_value());
  KEXPECT(found(*b, *fresh));

  // Only an original revokes, and only with Revoke.
  KEXPECT(b->revoke(*fresh).error() == Error::AccessDenied);
  const auto weak = a->insert(*probe, CapRights::ReadWrite);
  KREQUIRE(weak.has_value());
  KEXPECT(a->revoke(*weak).error() == Error::AccessDenied);

  // The dead ones still hold their slots until removed, and removing them works.
  KEXPECT(b->remove(*child).has_value());
  KEXPECT(c->remove(*grandchild).has_value());

  a.reset();
  b.reset();
  c.reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
}

KTEST(CSpace, the_last_capability_and_the_last_reference_each_end_something) {
  Counters counters;
  auto a = new_cspace();
  auto b = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(a && b && probe);

  const auto one = a->insert(*probe, CapRights::All);
  KREQUIRE(one.has_value());
  const auto two = a->copy(*one, *b);
  KREQUIRE(two.has_value());
  KEXPECT_EQ(probe->cap_count(), 2u);

  Probe* raw = probe.get();
  probe.reset();
  KEXPECT_EQ(raw->ref_count(), 2u);

  KEXPECT(a->remove(*one).has_value());
  KEXPECT_EQ(counters.unreachable.load(), 0u);
  KEXPECT(b->remove(*two).has_value());
  KEXPECT_EQ(counters.unreachable.load(), 1u);
  KEXPECT(core_probe::settle(counters, 1));

  // A CSpace that goes takes its capabilities' references along.
  auto more = new_probe(counters);
  KREQUIRE(more);
  for (int i = 0; i < 10; ++i) {
    KEXPECT(a->insert(*more, CapRights::Read).has_value());
  }

  KEXPECT_EQ(more->ref_count(), 11u);
  a.reset();
  b.reset();
  KEXPECT(support::wait_for([&] {
    core::ipc::reclaim_now();
    return more->ref_count() == 1u;
  }));
  KEXPECT_EQ(more->cap_count(), 0u);
  more.reset();
  KEXPECT(core_probe::settle(counters, 2));
}

KTEST(CSpace, it_fills_up_to_its_capacity_and_reuses_what_is_freed) {
  Counters counters;
  auto cs = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(cs && probe);

  // Slot 0 is what a null ID names, and never handed out.
  CapId in_node_3{};
  std::uint32_t inserted = 0;
  while (true) {
    const auto id = cs->insert(*probe, CapRights::Read);
    if (!id) {
      KEXPECT(id.error() == Error::OutOfMemory);
      break;
    }

    if (id->get_index() >> CSpace::NODE_BITS == 3 && in_node_3.is_null()) {
      in_node_3 = *id;
    }

    ++inserted;
  }

  KEXPECT_EQ(inserted, CSpace::MAX_CAPACITY - 1);
  KEXPECT_EQ(cs->active_count(), CSpace::MAX_CAPACITY - 1);

  KEXPECT(cs->remove(in_node_3).has_value());
  const auto reused = cs->insert(*probe, CapRights::Read);
  KREQUIRE(reused.has_value());
  KEXPECT_EQ(reused->get_index(), in_node_3.get_index());

  cs->clear();
  KEXPECT_EQ(cs->active_count(), 0u);
  KEXPECT_EQ(probe->ref_count(), 1u);
  KEXPECT_EQ(counters.unreachable.load(), 1u);

  cs.reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
}

KTEST(CSpace, an_address_that_cannot_be_packed_is_refused) {
  KEXPECT(ipc::CapHeader::is_representable(reinterpret_cast<void*>(0xFFFF'8000'0000'1230ull)));
  KEXPECT(ipc::CapHeader::is_representable(reinterpret_cast<void*>(0x0000'7FFF'FFFF'FFF0ull)));
  KEXPECT(!ipc::CapHeader::is_representable(reinterpret_cast<void*>(0x0000'8000'0000'0000ull)));
  KEXPECT(!ipc::CapHeader::is_representable(reinterpret_cast<void*>(0xFFFF'8000'0000'1238ull)));
  KEXPECT(!ipc::CapHeader::is_representable(reinterpret_cast<void*>(0xFF00'0000'0000'0000ull)));
}

// A copy keeps every ID: each finds the same object with the same rights, flags and badge, only
// never as the original, so the original can still revoke it. A one-shot capability stays where it
// was, and its ID finds nothing in the copy, not even whatever its slot gets there later.
KTEST(CSpace, a_copy_keeps_every_id_and_the_originals_keep_revoking) {
  Counters counters;
  auto from = new_cspace();
  auto to = new_cspace();
  auto probe = new_probe(counters);
  auto other = new_probe(counters);
  KREQUIRE(from && to && probe && other);

  const auto original = from->insert(*probe, CapRights::All);
  KREQUIRE(original.has_value());
  const auto badged = from->mint(*original, *from, CapRights::ReadWrite, 42);
  const auto pinned = from->insert(*other, CapRights::Read, CapFlags::Original | CapFlags::Pinned);
  const auto once = from->insert(*other, CapRights::Write, CapFlags::OneShot);
  const auto gone = from->insert(*other, CapRights::Read);
  KREQUIRE(badged && pinned && once && gone);
  KEXPECT(from->remove(*gone).has_value());

  KREQUIRE(from->copy_into(*to).has_value());
  KEXPECT_EQ(to->active_count(), from->active_count() - 1);
  KEXPECT_EQ(probe->cap_count(), 4u);

  for (const CapId id : {*original, *badged, *pinned}) {
    const auto here = look(*from, id);
    const auto there = look(*to, id);
    KREQUIRE(here.has_value() && there.has_value());
    KEXPECT(there->object() == here->object());
    KEXPECT(there->rights() == here->rights());
    KEXPECT_EQ(there->badge, here->badge);
    KEXPECT(!ipc::has_cap_flag(there->flags(), CapFlags::Original));
    KEXPECT_EQ(ipc::has_cap_flag(there->flags(), CapFlags::Pinned),
               ipc::has_cap_flag(here->flags(), CapFlags::Pinned));
  }

  KEXPECT(found(*from, *once));
  KEXPECT(!found(*to, *once));
  KEXPECT(!found(*to, *gone));

  for (int i = 0; i < 8; ++i) {
    KEXPECT(to->insert(*other, CapRights::Read).has_value());
  }

  KEXPECT(!found(*to, *once));

  // Revoking an original here ends what was copied from it there.
  KEXPECT(from->revoke(*original).has_value());
  KEXPECT(!found(*to, *original));
  KEXPECT(!found(*to, *badged));
  KEXPECT(found(*from, *original));

  from.reset();
  to.reset();
  probe.reset();
  other.reset();
  KEXPECT(core_probe::settle(counters, 2));
}

// A table of several nodes is copied whole, and the copy pays for its nodes. It only goes into an
// empty CSpace, and one that fails is left with nothing in it, its quota as it was.
KTEST(CSpace, a_copy_takes_every_node_and_pays_for_them) {
  Counters counters;
  auto from = new_cspace();
  auto probe = new_probe(counters);
  KREQUIRE(from && probe);

  // 768 of them, past slot 0 of node 0, which nothing gets: four nodes.
  constexpr std::uint32_t CAPS = 3 * CSpace::SLOTS_PER_NODE;
  constexpr std::size_t NODES = 4;
  std::array<CapId, CAPS> ids{};
  for (CapId& id : ids) {
    const auto inserted = from->insert(*probe, CapRights::Read);
    KREQUIRE(inserted.has_value());
    id = *inserted;
  }

  const auto quota = memory::QuotaRef::adopt(
      memory::Quota::create(sizeof(CSpace) + NODES * sizeof(ipc::CapTableNode)));
  KREQUIRE(quota.get() != nullptr);
  auto to = CSpace::create_charged(quota.get());
  KREQUIRE(to.has_value());

  KREQUIRE(from->copy_into(**to).has_value());
  KEXPECT_EQ(quota.get()->used(), sizeof(CSpace) + NODES * sizeof(ipc::CapTableNode));
  KEXPECT_EQ((*to)->active_count(), CAPS);
  for (const CapId id : ids) {
    const auto there = look(**to, id);
    KEXPECT(there.has_value() && there->object() == probe.get());
  }

  // Not into one that has something in it already.
  auto busy = new_cspace();
  KREQUIRE(busy && busy->insert(*probe, CapRights::Read).has_value());
  KEXPECT(from->copy_into(*busy).error() == Error::InvalidState);
  KEXPECT_EQ(busy->active_count(), 0u);

  // Nor past the quota of the one it goes into.
  const auto small = memory::QuotaRef::adopt(
      memory::Quota::create(sizeof(CSpace) + 2 * sizeof(ipc::CapTableNode)));
  KREQUIRE(small.get() != nullptr);
  auto cramped = CSpace::create_charged(small.get());
  KREQUIRE(cramped.has_value());
  KEXPECT(from->copy_into(**cramped).error() == Error::QuotaExceeded);
  KEXPECT_EQ((*cramped)->active_count(), 0u);

  from.reset();
  to->reset();
  busy.reset();
  cramped->reset();
  probe.reset();
  KEXPECT(core_probe::settle(counters, 1));
}

// CPU 0 makes objects and lets them go, mints and revokes, in one CSpace, as fast as it can, while
// every other CPU looks up whatever IDs it last saw, takes references to what it finds and copies
// it into a CSpace of its own. Every capability's badge carries its object's serial, which is
// never used twice: what a lookup finds has to be alive, and the object its badge says. A lookup
// that mixed up two capabilities, or an object freed under a reader, recycled or not, shows.
KTEST_ALL_CPUS(CSpace, lookups_stay_safe_while_capabilities_come_and_go, cpu) {
  constexpr Duration RUN = Duration::milliseconds(300);
  constexpr std::uint32_t IDS = 64;
  static Counters counters;
  static CSpace* shared = nullptr;
  static std::array<std::atomic<std::uint32_t>, IDS> ids{};
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> created{0};
  static std::atomic<std::uint64_t> broken{0};
  static std::atomic<std::uint64_t> lookups{0};
  static std::atomic<std::uint64_t> changes{0};

  if (cpu == 0) {
    counters.destroyed.store(0);
    counters.unreachable.store(0);
    shared = new_cspace().leak();
    for (auto& id : ids) {
      id.store(0);
    }

    stop.store(false);
    created.store(0);
    broken.store(0);
    lookups.store(0);
    changes.store(0);
  }

  sync_cpus();

  // Whether what a lookup found is what it should be.
  const auto intact = [](const ipc::CapEntry& entry) noexcept {
    const auto* probe = static_cast<const Probe*>(entry.object());
    return probe->alive() && probe->serial == entry.badge >> 16;
  };

  support::Rng rng{0xC5 + cpu};
  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t local = 0;
    std::uint64_t serial = 0;
    while (drivers::SystemClock::now() < end) {
      const std::uint32_t slot = static_cast<std::uint32_t>(rng.below(IDS));
      const CapId current{ids[slot].load(std::memory_order_relaxed)};
      switch (rng.below(4)) {
        case 0: {
          // A new object, its original here, and nothing else holding it.
          auto probe = Probe::create(counters, ++serial);
          if (!probe) {
            break;
          }

          const auto id =
              shared->insert(**probe, CapRights::All, CapFlags::Original, (serial << 16) | 1);
          if (id) {
            if (const std::uint32_t old = ids[slot].exchange(id->raw())) {
              (void)shared->remove(CapId{old});
            }
          }

          created.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        case 1:
          // A derived one, in place of whatever was here: the object may go with this.
          if (!current.is_null()) {
            if (const auto id = shared->copy(current, *shared, CapRights::ReadWrite)) {
              if (const std::uint32_t old = ids[slot].exchange(id->raw())) {
                (void)shared->remove(CapId{old});
              }
            }
          }
          break;
        case 2:
          if (const std::uint32_t old = ids[slot].exchange(0, std::memory_order_acq_rel)) {
            (void)shared->remove(CapId{old});
          }
          break;
        default:
          (void)shared->revoke(current);
          break;
      }

      // Every CPU runs a test body here, and those are idle threads, which nothing preempts: the
      // worker that destroys dead objects gets no CPU until this is over. So this does its job.
      if (++local % 32 == 0) {
        ipc::reclaim_now();
      }
    }

    changes.store(local, std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
  } else {
    auto mine = new_cspace();
    std::uint64_t local_lookups = 0;
    std::uint64_t local_broken = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const CapId id{ids[rng.below(IDS)].load(std::memory_order_acquire)};
      {
        // Half the time, in the read section for a while before looking, as code that uses what
        // it found would be: long enough for an object freed too early to be gone by then.
        const utils::rcu::ReadGuard rcu;
        if (const auto entry = shared->lookup(id, rcu)) {
          if (rng.below(2) == 0) {
            support::spin_for(Duration::microseconds(static_cast<std::int64_t>(rng.below(50))));
          }

          local_broken += intact(*entry) ? 0 : 1;
        }
      }

      if (const auto held = shared->acquire<Probe>(id, CapRights::Read)) {
        local_broken += held->object->alive() && held->object->serial == held->badge >> 16 ? 0 : 1;
        if (const auto copied = shared->copy(id, *mine, CapRights::Read)) {
          (void)mine->remove(*copied);
        }
      }

      ++local_lookups;
    }

    lookups.fetch_add(local_lookups, std::memory_order_relaxed);
    broken.fetch_add(local_broken, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(broken.load(), 0u);
    KEXPECT_GT(changes.load(), 1000u);

    // Every object, once its capabilities are gone, goes exactly once.
    shared->release_ref();
    KEXPECT(core_probe::settle(counters, static_cast<std::uint32_t>(created.load())));
    KEXPECT_EQ(counters.destroyed.load(), created.load());
    utils::logger::info("CSpace: {} changes, {} objects, {} lookups alongside\n", changes.load(),
                        created.load(), lookups.load());
  }

  sync_cpus();
}
}  // namespace kernel::testing
