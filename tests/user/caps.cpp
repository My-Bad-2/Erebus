// The Cap family from user code: making objects, and copying, badging, moving and revoking what
// names them.
#include <cstdint>

#include "ktest.hpp"
#include "support.hpp"
#include "sys/invoker.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
using sys::CapId;
using sys::CapRights;
using sys::Error;
using sys::NULL_CAP;
}  // namespace

KTEST(Capabilities, create_makes_what_can_be_made_from_nothing) {
  const auto endpoint = sys::cap::create_endpoint();
  const auto notification = sys::cap::create_notification();
  const auto cspace = sys::cap::create_cspace();
  const auto object = sys::cap::create_vmo(4096);
  KEXPECT_OK(endpoint);
  KEXPECT_OK(notification);
  KEXPECT_OK(cspace);
  KEXPECT_OK(object);

  for (const auto& made : {endpoint, notification, cspace, object}) {
    if (made) {
      KEXPECT_NE(*made, NULL_CAP);
      KEXPECT_OK(sys::cap::remove(NULL_CAP, *made));
    }
  }

  // The rest stand for something only the kernel can hand out.
  for (const sys::CapType type :
       {sys::CapType::Null, sys::CapType::Reply, sys::CapType::Mmio, sys::CapType::IrqHandler,
        sys::CapType::DmaPool, sys::CapType::Thread, sys::CapType::Process}) {
    KEXPECT_ERROR(sys::cap::create(type), Error::InvalidArgument);
  }
  KEXPECT_ERROR(sys::SyscallInvoker::invoke_as<CapId>(sys::Syscall::CapCreate, 0x101),
                Error::InvalidArgument);
}

KTEST(Capabilities, an_id_that_was_deleted_names_nothing) {
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(notification);
  KREQUIRE_OK(sys::cap::remove(NULL_CAP, *notification));

  KEXPECT_ERROR(sys::ipc::signal(*notification), Error::NotFound);
  KEXPECT_ERROR(sys::cap::remove(NULL_CAP, *notification), Error::NotFound);

  // The slot may be used again, under another generation: the old ID still names nothing.
  const auto next = sys::cap::create_notification();
  KREQUIRE_OK(next);
  KEXPECT_NE(*next, *notification);
  KEXPECT_ERROR(sys::ipc::signal(*notification), Error::NotFound);
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *next));

  // And 0 is no capability at all.
  KEXPECT_ERROR(sys::ipc::signal(NULL_CAP), Error::InvalidArgument);
}

KTEST(Capabilities, a_copy_has_no_more_rights_than_it_was_given) {
  const auto original = sys::cap::create_notification();
  KREQUIRE_OK(original);
  const auto reader = sys::cap::copy(NULL_CAP, *original, NULL_CAP, CapRights::Read);
  KREQUIRE_OK(reader);

  KEXPECT_ERROR(sys::ipc::signal(*reader), Error::AccessDenied);
  KEXPECT_OK(sys::ipc::signal(*original));
  KEXPECT_EQ(sys::ipc::poll(*reader).value_or(0), 1u);

  // Rights it does not have cannot come back by minting.
  KEXPECT_ERROR(sys::cap::mint(NULL_CAP, *reader, NULL_CAP, CapRights::ReadWrite),
                Error::PrivilegeEscalationDetected);
  KEXPECT_ERROR(sys::SyscallInvoker::invoke_as<CapId>(sys::Syscall::CapCopy, NULL_CAP, *original,
                                                      NULL_CAP, 0x100),
                Error::InvalidFlags);

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *reader));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *original));
}

// A signal sets the bits of the capability's badge, so a waiter can tell who signalled.
KTEST(Capabilities, a_minted_badge_is_what_its_signals_set_and_stays) {
  const auto original = sys::cap::create_notification();
  KREQUIRE_OK(original);
  const auto first = sys::cap::mint(NULL_CAP, *original, NULL_CAP, CapRights::Write, 0x10);
  const auto second = sys::cap::mint(NULL_CAP, *original, NULL_CAP, CapRights::Write, 0x4);
  KREQUIRE_OK(first);
  KREQUIRE_OK(second);

  KEXPECT_OK(sys::ipc::signal(*first));
  KEXPECT_OK(sys::ipc::signal(*second));
  KEXPECT_EQ(sys::ipc::poll(*original).value_or(0), 0x14u);
  KEXPECT_EQ(sys::ipc::poll(*original).value_or(1), 0u);

  // A badged capability keeps its badge: another one may not be put on a copy, and without Mint
  // no badge may be put on at all.
  const auto rebadged_by_mint = sys::cap::mint(NULL_CAP, *first, NULL_CAP, CapRights::Write, 0x20);
  KEXPECT(!rebadged_by_mint);
  const auto plain = sys::cap::copy(NULL_CAP, *original, NULL_CAP, CapRights::ReadWrite);
  KREQUIRE_OK(plain);
  KEXPECT_ERROR(sys::cap::mint(NULL_CAP, *plain, NULL_CAP, CapRights::Write, 0x40),
                Error::AccessDenied);

  for (const CapId id : {*plain, *second, *first, *original}) {
    KEXPECT_OK(sys::cap::remove(NULL_CAP, id));
  }
}

KTEST(Capabilities, a_badge_cannot_be_changed_by_one_that_may_mint) {
  const auto original = sys::cap::create_notification();
  KREQUIRE_OK(original);
  const auto badged = sys::cap::mint(NULL_CAP, *original, NULL_CAP, CapRights::All, 0x1);
  KREQUIRE_OK(badged);

  KEXPECT_ERROR(sys::cap::mint(NULL_CAP, *badged, NULL_CAP, CapRights::Write, 0x2),
                Error::SecurityViolation);
  // The same badge again is no change.
  const auto same = sys::cap::mint(NULL_CAP, *badged, NULL_CAP, CapRights::Write, 0x1);
  KEXPECT_OK(same);

  if (same) {
    KEXPECT_OK(sys::cap::remove(NULL_CAP, *same));
  }
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *badged));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *original));
}

KTEST(Capabilities, revoke_ends_everything_derived_from_the_original) {
  const auto original = sys::cap::create_notification();
  KREQUIRE_OK(original);
  const auto copy = sys::cap::copy(NULL_CAP, *original, NULL_CAP);
  KREQUIRE_OK(copy);
  const auto copy_of_copy = sys::cap::copy(NULL_CAP, *copy, NULL_CAP);
  KREQUIRE_OK(copy_of_copy);

  const auto revoked = sys::cap::revoke(NULL_CAP, *original);
  KEXPECT_OK(revoked);
  KEXPECT_EQ(revoked.value_or(0), 2u);

  KEXPECT_ERROR(sys::ipc::signal(*copy), Error::NotFound);
  KEXPECT_ERROR(sys::ipc::signal(*copy_of_copy), Error::NotFound);
  KEXPECT_OK(sys::ipc::signal(*original));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *original));
}

KTEST(Capabilities, move_takes_one_from_a_cspace_to_another) {
  const auto other = sys::cap::create_cspace();
  const auto notification = sys::cap::create_notification();
  KREQUIRE_OK(other);
  KREQUIRE_OK(notification);

  const auto there = sys::cap::move(NULL_CAP, *notification, *other);
  KREQUIRE_OK(there);
  KEXPECT_ERROR(sys::ipc::signal(*notification), Error::NotFound);

  const auto back = sys::cap::move(*other, *there, NULL_CAP);
  KREQUIRE_OK(back);
  KEXPECT_OK(sys::ipc::signal(*back));

  // Taking out of a CSpace takes rights on it.
  const auto read_only = sys::cap::copy(NULL_CAP, *other, NULL_CAP, CapRights::Read);
  KREQUIRE_OK(read_only);
  KEXPECT_ERROR(sys::cap::move(NULL_CAP, *back, *read_only), Error::AccessDenied);

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *read_only));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *back));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *other));
}

KTEST(Capabilities, a_pinned_one_stays_until_it_is_revoked) {
  const auto original = sys::cap::create_notification();
  KREQUIRE_OK(original);
  const auto pinned =
      sys::cap::mint(NULL_CAP, *original, NULL_CAP, CapRights::All, 0, sys::CapFlags::Pinned);
  KREQUIRE_OK(pinned);

  KEXPECT_ERROR(sys::cap::remove(NULL_CAP, *pinned), Error::AccessDenied);
  const auto other = sys::cap::create_cspace();
  KREQUIRE_OK(other);
  KEXPECT_ERROR(sys::cap::move(NULL_CAP, *pinned, *other), Error::AccessDenied);

  KEXPECT_EQ(sys::cap::revoke(NULL_CAP, *original).value_or(0), 1u);
  KEXPECT_ERROR(sys::ipc::signal(*pinned), Error::NotFound);

  KEXPECT_OK(sys::cap::remove(NULL_CAP, *other));
  KEXPECT_OK(sys::cap::remove(NULL_CAP, *original));
}
}  // namespace user::testing
