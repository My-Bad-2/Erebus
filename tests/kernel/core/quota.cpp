// Quotas on what user code can have the kernel hold: how charges come and go, the objects that
// carry them, and the processes that share them.
#include "memory/quota.hpp"

#include <array>
#include <cstddef>
#include <utility>

#include "../sched/support.hpp"
#include "core/cspace.hpp"
#include "core/ipc/driver_caps.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/notification.hpp"
#include "core/ipc/object_cap.hpp"
#include "hal/fpu.hpp"
#include "ktest.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm/vma/vm_object.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "proc/process.hpp"
#include "sched/thread.hpp"
#include "utils/maths.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::testing {
namespace {
namespace ipc = core::ipc;
using ipc::CapRef;
using ipc::CapRights;
using memory::PAGE_SIZE;
using memory::Quota;
using memory::QuotaCharge;
using memory::QuotaRef;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;

constexpr VirtualAddress USER_BASE{0x0000100000000000ul};
constexpr AccessFlags USER_RW = AccessFlags::Read | AccessFlags::Write | AccessFlags::User;

template <typename T>
[[nodiscard]] CapRef<T> made(std::expected<CapRef<T>, Error> result) noexcept {
  KEXPECT(result.has_value());
  return result ? std::move(*result) : CapRef<T>{};
}

// Objects give their charge back when they are destroyed, a grace period after their last
// reference, and a destructor may let go of more. So this waits for the count, not for a barrier.
[[nodiscard]] bool settles_to(const Quota& quota, const std::size_t used) noexcept {
  utils::rcu::barrier();
  return support::wait_for([&] {
    ipc::reclaim_now();
    return quota.used() == used;
  });
}

// What a SharedVmObject of @p pages pages charges: itself, its VM object, and every page.
constexpr std::size_t shared_object_bytes(const std::size_t pages) noexcept {
  return sizeof(ipc::SharedVmObject) + sizeof(memory::vmm::VMObject) + pages * PAGE_SIZE;
}
}  // namespace

KTEST(Quota, charges_stop_at_the_limit_and_come_back) {
  const QuotaRef quota = QuotaRef::adopt(Quota::create(1000));
  KREQUIRE(quota.get() != nullptr);
  Quota& q = *quota.get();

  KEXPECT(q.charge(600).has_value());
  KEXPECT(q.charge(401).error() == Error::QuotaExceeded);
  KEXPECT_EQ(q.used(), 600u);
  KEXPECT(q.charge(400).has_value());
  KEXPECT(q.charge(1).error() == Error::QuotaExceeded);

  q.uncharge(1000);
  KEXPECT_EQ(q.used(), 0u);

  // So big that adding it would wrap around is past the limit too.
  KEXPECT(q.charge(~std::size_t{0}).error() == Error::QuotaExceeded);
  KEXPECT_EQ(q.used(), 0u);
}

KTEST(Quota, a_charge_holds_its_bytes_until_it_goes) {
  const QuotaRef quota = QuotaRef::adopt(Quota::create(PAGE_SIZE));
  KREQUIRE(quota.get() != nullptr);
  Quota& q = *quota.get();

  {
    auto charge = QuotaCharge::take(&q, 100);
    KREQUIRE(charge.has_value());
    KEXPECT(charge->quota() == &q);

    KEXPECT(charge->grow(50).has_value());
    KEXPECT(charge->grow(PAGE_SIZE).error() == Error::QuotaExceeded);
    KEXPECT_EQ(charge->bytes(), 150u);
    KEXPECT_EQ(q.used(), 150u);

    charge->shrink(50);
    KEXPECT_EQ(q.used(), 100u);

    // Moving it moves the bytes, and the one moved from has nothing left to give back.
    const QuotaCharge moved = std::move(*charge);
    KEXPECT_EQ(moved.bytes(), 100u);
    KEXPECT_EQ(charge->bytes(), 0u);
    KEXPECT(charge->quota() == nullptr);
    KEXPECT_EQ(q.used(), 100u);
  }

  KEXPECT_EQ(q.used(), 0u);
  KEXPECT(QuotaCharge::take(&q, PAGE_SIZE + 1).error() == Error::QuotaExceeded);

  // The kernel's own objects count against nothing, so nothing refuses them.
  auto unlimited = QuotaCharge::take(nullptr, ~std::size_t{0});
  KREQUIRE(unlimited.has_value());
  KEXPECT(unlimited->quota() == nullptr);
  KEXPECT(unlimited->grow(~std::size_t{0}).has_value());
}

KTEST(Quota, objects_count_until_they_are_gone) {
  constexpr std::size_t NOTIFICATIONS = 3;
  const QuotaRef quota = QuotaRef::adopt(
      Quota::create(sizeof(ipc::Endpoint) + NOTIFICATIONS * sizeof(ipc::Notification)));
  KREQUIRE(quota.get() != nullptr);
  Quota& q = *quota.get();

  auto endpoint = made(ipc::Endpoint::create_charged(&q));
  KREQUIRE(endpoint);
  KEXPECT(endpoint->charged_to() == &q);
  KEXPECT_EQ(endpoint->charged(), sizeof(ipc::Endpoint));
  KEXPECT_EQ(q.used(), sizeof(ipc::Endpoint));

  std::array<CapRef<ipc::Notification>, NOTIFICATIONS> notifications{};
  for (auto& notification : notifications) {
    notification = made(ipc::Notification::create_charged(&q));
  }

  // Full: one more is refused, and leaves nothing behind.
  KEXPECT(ipc::Notification::create_charged(&q).error() == Error::QuotaExceeded);
  KEXPECT_EQ(q.used(), sizeof(ipc::Endpoint) + NOTIFICATIONS * sizeof(ipc::Notification));

  // What the kernel makes for itself counts against nothing.
  const auto own = made(ipc::Notification::create());
  KEXPECT(own->charged_to() == nullptr);

  // A capability keeps an object, and so its charge, after the reference it was made with is gone.
  auto cspace = made(ipc::CSpace::create());
  KREQUIRE(cspace->insert(*notifications[0], CapRights::All).has_value());
  notifications[0].reset();
  endpoint.reset();
  KEXPECT(settles_to(q, NOTIFICATIONS * sizeof(ipc::Notification)));

  cspace.reset();
  for (auto& notification : notifications) {
    notification.reset();
  }

  KEXPECT(settles_to(q, 0));
}

KTEST(Quota, a_cspace_pays_for_its_table_as_it_grows) {
  const QuotaRef quota =
      QuotaRef::adopt(Quota::create(sizeof(ipc::CSpace) + sizeof(ipc::CapTableNode)));
  KREQUIRE(quota.get() != nullptr);
  Quota& q = *quota.get();

  auto cspace = made(ipc::CSpace::create_charged(&q));
  KREQUIRE(cspace);
  KEXPECT_EQ(q.used(), sizeof(ipc::CSpace));

  // The first node takes the rest. Its slot 0 is never handed out, as that would be the null ID.
  const auto object = made(ipc::Notification::create());
  std::array<ipc::CapId, ipc::CSpace::SLOTS_PER_NODE - 1> ids{};
  for (ipc::CapId& id : ids) {
    const auto inserted = cspace->insert(*object, CapRights::All);
    KREQUIRE(inserted.has_value());
    id = *inserted;
  }

  KEXPECT_EQ(q.used(), sizeof(ipc::CSpace) + sizeof(ipc::CapTableNode));

  // A second node would be past the limit. A slot that comes free again needs none.
  KEXPECT(cspace->insert(*object, CapRights::All).error() == Error::QuotaExceeded);
  KEXPECT(cspace->remove(ids[7]).has_value());
  KEXPECT(cspace->insert(*object, CapRights::All).has_value());
  KEXPECT_EQ(q.used(), sizeof(ipc::CSpace) + sizeof(ipc::CapTableNode));

  cspace.reset();
  KEXPECT(settles_to(q, 0));
}

KTEST(Quota, shared_memory_counts_every_page_while_anything_can_reach_it) {
  constexpr CapRights GROW = CapRights::Write | CapRights::Manage;
  const QuotaRef quota = QuotaRef::adopt(Quota::create(64 * PAGE_SIZE));
  KREQUIRE(quota.get() != nullptr);
  Quota& q = *quota.get();

  // Pages come on demand, so all of them count from the start: this one never fits.
  KEXPECT(ipc::SharedVmObject::create(64 * PAGE_SIZE, memory::vmm::PageSize::Size4K, &q).error() ==
          Error::QuotaExceeded);
  KEXPECT_EQ(q.used(), 0u);

  auto shared = made(ipc::SharedVmObject::create(8 * PAGE_SIZE, memory::vmm::PageSize::Size4K, &q));
  KREQUIRE(shared);
  KEXPECT_EQ(q.used(), shared_object_bytes(8));

  // Growing pays for the pages added, and does not happen past the limit.
  KEXPECT(shared->resize(GROW, 16).has_value());
  KEXPECT_EQ(q.used(), shared_object_bytes(16));
  KEXPECT(shared->resize(GROW, 1000).error() == Error::QuotaExceeded);
  KEXPECT_EQ(shared->pages(), 16u);
  KEXPECT_EQ(q.used(), shared_object_bytes(16));

  // A window onto part of it pays for itself; its pages count already.
  auto slice = made(shared->derive(CapRights::Mint, 2, 4, &q));
  KREQUIRE(slice);
  KEXPECT_EQ(q.used(), shared_object_bytes(16) + sizeof(ipc::SharedVmObject));

  // The pages stay as long as the window does, and so does their charge.
  shared.reset();
  KEXPECT(settles_to(q, shared_object_bytes(16)));

  slice.reset();
  KEXPECT(settles_to(q, 0));
}

KTEST(Quota, a_tree_of_processes_shares_one) {
  constexpr CapRights MANAGE = CapRights::Write | CapRights::Manage;

  // Process 0 counts nothing; each process it starts begins a tree with a quota of its own.
  KEXPECT(proc::kernel_process()->quota() == nullptr);
  auto first = made(ipc::ProcessCap::create("quota-first"));
  auto second = made(ipc::ProcessCap::create("quota-second"));
  KREQUIRE(first && second);
  Quota* const tree = first->process().quota();
  KREQUIRE(tree != nullptr);
  KEXPECT(second->process().quota() != nullptr && second->process().quota() != tree);

  // Kept past the processes, to see that everything they were charged comes back.
  const QuotaRef keep = QuotaRef::share(tree);

  // A process another one makes is in its tree, and spends the same quota.
  auto child = made(ipc::ProcessCap::create("quota-child", &first->process()));
  KREQUIRE(child);
  KEXPECT(child->process().quota() == tree);

  // So does the CSpace each of them gets.
  KEXPECT(first->cspace().charged_to() == tree);
  KEXPECT(child->cspace().charged_to() == tree);
  KEXPECT(child->charged_to() == tree);

  const proc::ProcessId child_pid = child->pid();
  KEXPECT(child->terminate(MANAGE, 0).has_value());
  KEXPECT(first->process().wait_child(child_pid).has_value());
  KEXPECT(first->terminate(MANAGE, 0).has_value());
  KEXPECT(second->terminate(MANAGE, 0).has_value());
  KEXPECT(proc::kernel_process()->wait_child(first->pid()).has_value());
  KEXPECT(proc::kernel_process()->wait_child(second->pid()).has_value());

  child.reset();
  first.reset();
  second.reset();
  KEXPECT(settles_to(*tree, 0));
}

KTEST(Quota, a_charge_counts_against_every_quota_above_too) {
  const QuotaRef parent = QuotaRef::adopt(Quota::create(1000));
  KREQUIRE(parent.get() != nullptr);
  const QuotaRef left = QuotaRef::adopt(Quota::create(800, parent.get()));
  const QuotaRef right = QuotaRef::adopt(Quota::create(800, parent.get()));
  KREQUIRE(left.get() != nullptr && right.get() != nullptr);
  KEXPECT(left.get()->parent() == parent.get());

  KEXPECT(left.get()->charge(600).has_value());
  KEXPECT_EQ(parent.get()->used(), 600u);

  // Room in its own quota, but not above it: refused, and counted nowhere.
  KEXPECT(right.get()->charge(600).error() == Error::QuotaExceeded);
  KEXPECT_EQ(right.get()->used(), 0u);
  KEXPECT_EQ(parent.get()->used(), 600u);

  // Past its own limit is past it, too, whatever is left above.
  KEXPECT(right.get()->charge(400).has_value());
  right.get()->uncharge(400);
  KEXPECT(left.get()->charge(201).error() == Error::QuotaExceeded);
  KEXPECT_EQ(parent.get()->used(), 600u);

  left.get()->uncharge(600);
  KEXPECT_EQ(parent.get()->used(), 0u);
}

KTEST(Quota, every_tree_is_under_a_root_sized_by_memory) {
  Quota* const root = Quota::root();
  KREQUIRE(root != nullptr);
  const std::size_t total = memory::pmm::total_bytes();
  KEXPECT_EQ(root->limit(), Quota::root_limit_for(total));
  KEXPECT(root->parent() == nullptr);
  KEXPECT(Quota::tree_limit_for(total) < root->limit());

  const QuotaRef tree = QuotaRef::adopt(Quota::create_tree());
  KREQUIRE(tree.get() != nullptr);
  KEXPECT_EQ(tree.get()->limit(), Quota::tree_limit_for(total));
  KEXPECT(tree.get()->parent() == root);

  // Objects of other tests may still be on their way out, which only ever lowers what the root
  // counts. So a charge big enough to tell apart from that, and ranges rather than equalities.
  constexpr std::size_t CHARGE = std::size_t{64} << 20;
  const std::size_t before = root->used();
  KEXPECT(tree.get()->charge(CHARGE).has_value());
  const std::size_t during = root->used();
  KEXPECT(during <= before + CHARGE);
  KEXPECT(during >= before + CHARGE / 2);

  tree.get()->uncharge(CHARGE);
  KEXPECT(root->used() <= during - CHARGE);
}

KTEST(Quota, anonymous_memory_counts_what_it_may_come_to) {
  const QuotaRef quota = QuotaRef::adopt(Quota::create(64 * PAGE_SIZE));
  const QuotaRef other = QuotaRef::adopt(Quota::create(64 * PAGE_SIZE));
  KREQUIRE(quota.get() != nullptr && other.get() != nullptr);
  Quota& q = *quota.get();

  auto* space = new memory::vmm::AddressSpace(USER_BASE);
  KREQUIRE(space != nullptr);
  space->set_quota(&q);

  // A range counts all of itself from the start, before any of it is touched.
  auto range = space->alloc(16 * PAGE_SIZE, USER_RW);
  KREQUIRE(range.has_value());
  KEXPECT_EQ(q.used(), 16 * PAGE_SIZE);

  KEXPECT(space->alloc(64 * PAGE_SIZE, USER_RW).error() == Error::QuotaExceeded);
  KEXPECT_EQ(q.used(), 16 * PAGE_SIZE);

  // Growing counts what is added, wherever the range ends up.
  range = space->realloc(*range, 24 * PAGE_SIZE);
  KREQUIRE(range.has_value());
  KEXPECT_EQ(q.used(), 24 * PAGE_SIZE);

  // A stack counts what it has, and what it grows by when it does.
  const auto stack = space->alloc(2 * PAGE_SIZE, USER_RW | AccessFlags::Stack);
  KREQUIRE(stack.has_value());
  KEXPECT_EQ(q.used(), 26 * PAGE_SIZE);
  const VirtualAddress below = *stack - static_cast<VirtualAddress::OffsetType>(PAGE_SIZE);
  KEXPECT(space->handle_page_fault(below, AccessFlags::Write).has_value());
  KEXPECT_EQ(q.used(), 27 * PAGE_SIZE);

  // After a fork either side may copy all of it, so each side counts all of it again.
  auto* child = new memory::vmm::AddressSpace(USER_BASE);
  KREQUIRE(child != nullptr);
  child->set_quota(other.get());
  KREQUIRE(space->clone_into(*child).has_value());
  KEXPECT_EQ(other.get()->used(), 27 * PAGE_SIZE);
  KEXPECT_EQ(q.used(), 2 * 27 * PAGE_SIZE);

  // And gives it back when it is gone.
  delete child;
  KEXPECT_EQ(other.get()->used(), 0u);
  KEXPECT(space->free(*range).has_value());
  KEXPECT(space->free_containing(below).has_value());
  KEXPECT_EQ(q.used(), 0u);
  delete space;
}

KTEST(Quota, a_process_and_its_threads_count_against_their_tree) {
  const auto created = proc::Process::create("quota-threads");
  KREQUIRE(created.has_value());
  proc::Process& process = **created;
  Quota* const tree = process.quota();
  KREQUIRE(tree != nullptr);
  KEXPECT(process.address_space()->quota() == tree);
  const QuotaRef keep = QuotaRef::share(tree);

  constexpr std::size_t FOOTPRINT =
      sizeof(proc::Process) + sizeof(memory::vmm::AddressSpace) + PAGE_SIZE;
  KEXPECT_EQ(tree->used(), FOOTPRINT);

  // Made and never started, so it never gets to ring 3. The kernel's part of it counts against the
  // tree, and so does all of its user stack, as memory of the address space.
  const sched::UserThreadCreateOptions opts{};
  const auto thread =
      sched::Thread::create_user("quota-thread", &process, VirtualAddress{0x400000}, 0, opts);
  KREQUIRE(thread.has_value());
  const std::size_t kernel_part = sizeof(sched::Thread) +
                                  utils::maths::align_up(opts.kernel_stack_size, PAGE_SIZE) +
                                  hw::fpu::area_size(false);
  const std::size_t user_stack = utils::maths::align_up(opts.user_stack_size, PAGE_SIZE);
  KEXPECT_EQ(tree->used(), FOOTPRINT + kernel_part + user_stack);

  KEXPECT(sched::Thread::destroy(*thread).has_value());
  KEXPECT_EQ(tree->used(), FOOTPRINT);

  const proc::ProcessId pid = process.pid();
  process.exit(0);
  KEXPECT(proc::kernel_process()->wait_child(pid).has_value());
  KEXPECT(settles_to(*tree, 0));
}
}  // namespace kernel::testing
