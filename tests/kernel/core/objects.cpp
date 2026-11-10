// The objects drivers, threads and processes are handed as capabilities: interrupts, registers,
// DMA buffers, shared memory, threads and processes, and the limits each of them keeps.
#include <array>
#include <atomic>
#include <chrono.hpp>
#include <cstdint>

#include "../sched/support.hpp"
#include "core/cspace.hpp"
#include "core/ipc/driver_caps.hpp"
#include "core/ipc/endpoint.hpp"
#include "core/ipc/notification.hpp"
#include "core/ipc/object_cap.hpp"
#include "hal/apic/lapic.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "ktest.hpp"
#include "memory/heap.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "sched/sleep.hpp"

namespace kernel::testing {
namespace {
namespace ipc = core::ipc;
using ipc::CapId;
using ipc::CapRef;
using ipc::CapRights;
using ipc::CSpace;
using ipc::Endpoint;
using ipc::Message;
using klib::Duration;
using memory::PAGE_SIZE;
using memory::PhysicalAddress;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;
using support::A_MOMENT;
using support::start;
using support::wait_for;

// Nobody else uses it; see tests/kernel/hal/interrupts.cpp for the ones next to it.
constexpr std::uint8_t IRQ_TEST_VECTOR = 0xCB;
constexpr VirtualAddress USER_BASE{0x0000100000000000ul};

template <typename T>
[[nodiscard]] CapRef<T> made(std::expected<CapRef<T>, Error> result) noexcept {
  KEXPECT(result.has_value());
  return result ? std::move(*result) : CapRef<T>{};
}

// Everything let go of so far is destroyed by the time this returns.
void settle() noexcept {
  utils::rcu::barrier();
  ipc::reclaim_now();
}

// Device registers every machine has, and no RAM: the local APIC's.
[[nodiscard]] PhysicalAddress lapic_base() noexcept {
  return PhysicalAddress{
      hw::apic::BaseMsrSchema{hw::read::msr(hw::apic::IA32_APIC_BASE)}.get_base_address()
      << memory::PAGE_SHIFT_4KB};
}
}  // namespace

// A device interrupt arrives as a signal, with the badge it was given, and stops arriving once the
// handler is gone. Vectors that are not meant for devices are refused.
KTEST(IrqHandler, an_interrupt_arrives_as_a_signal) {
  KEXPECT(ipc::IrqHandler::create_vector(14).error() == Error::InvalidArgument);
  KEXPECT(ipc::IrqHandler::create_vector(0xFD).error() == Error::InvalidArgument);

  auto notification = made(ipc::Notification::create());
  auto handler = made(ipc::IrqHandler::create_vector(IRQ_TEST_VECTOR));
  KREQUIRE(notification && handler);

  KEXPECT(handler->set_notification(CapRights::Read, *notification, 8).error() ==
          Error::AccessDenied);
  KEXPECT(handler->set_notification(CapRights::Manage, *notification, 0).error() ==
          Error::InvalidArgument);
  KEXPECT(handler->set_notification(CapRights::Manage, *notification, 8).has_value());

  hw::percpu::lapic().send_self_ipi(IRQ_TEST_VECTOR);
  const auto bits = notification->wait_for(Duration::seconds(2));
  KEXPECT(bits && *bits == 8);
  KEXPECT_EQ(handler->irq_count(), 1u);
  KEXPECT(handler->acknowledge(CapRights::Manage).has_value());

  KEXPECT(handler->clear_notification(CapRights::Manage).has_value());
  hw::percpu::lapic().send_self_ipi(IRQ_TEST_VECTOR);
  support::spin_for(Duration::milliseconds(1));
  KEXPECT_EQ(handler->irq_count(), 2u);
  KEXPECT_EQ(notification->poll(), 0u);

  // Gone, it takes its registration along: the vector is nobody's again.
  handler.reset();
  settle();
  hw::percpu::lapic().send_self_ipi(IRQ_TEST_VECTOR);
  support::spin_for(Duration::milliseconds(1));
}

// Device registers, never RAM, and nothing past the end of the region, however it is asked for.
KTEST(MmioRegion, maps_registers_and_never_ram) {
  void* ram = memory::heap::kmalloc(PAGE_SIZE, PAGE_SIZE);
  KREQUIRE(ram != nullptr);
  const auto ram_phys =
      memory::DirectMap::virt_to_phys(VirtualAddress{reinterpret_cast<std::uintptr_t>(ram)});
  KREQUIRE(ram_phys.has_value());
  KEXPECT(ipc::MmioRegion::create(*ram_phys, PAGE_SIZE).error() == Error::SecurityViolation);
  memory::heap::kfree(ram);

  KEXPECT(ipc::MmioRegion::create(PhysicalAddress{0xFEE00010}, PAGE_SIZE).error() ==
          Error::InvalidAlignment);
  KEXPECT(ipc::MmioRegion::create(PhysicalAddress{~std::uint64_t{0xFFF}}, 2 * PAGE_SIZE).error() ==
          Error::OutOfBounds);

  const PhysicalAddress lapic = lapic_base();
  auto region = made(ipc::MmioRegion::create(lapic, PAGE_SIZE));
  KREQUIRE(region);

  KEXPECT(region->derive(CapRights::Read, 0, PAGE_SIZE, region->cache()).error() ==
          Error::AccessDenied);
  KEXPECT(region->derive(CapRights::Mint, 0, 2 * PAGE_SIZE, region->cache()).error() ==
          Error::OutOfBounds);
  KEXPECT(region->derive(CapRights::Mint, PAGE_SIZE, PAGE_SIZE, region->cache()).error() ==
          Error::OutOfBounds);
  KEXPECT(region->derive(CapRights::Mint, 0, ~std::size_t{0}, region->cache()).error() ==
          Error::OutOfBounds);

  auto* space = new memory::vmm::AddressSpace(USER_BASE);
  KEXPECT(region->map_into(*space, CapRights::Write).error() == Error::AccessDenied);
  KEXPECT(region->map_into(*space, CapRights::Read, 0, 2 * PAGE_SIZE).error() ==
          Error::OutOfBounds);
  KEXPECT(region->map_into(*space, CapRights::Read, PAGE_SIZE).error() == Error::OutOfBounds);

  const auto mapped = region->map_into(*space, CapRights::Read);
  KREQUIRE(mapped.has_value());
  const auto found = space->pagemap().translate(*mapped);
  KEXPECT(found && found->phys_address == lapic);
  KEXPECT(found && !memory::vmm::has_flag(found->flags, AccessFlags::Write));
  delete space;
}

// Shared memory: two mappings of one object see the same bytes; a part of it sees only its part,
// and no offset, however large, gets anything outside.
KTEST(SharedVmObject, mappings_share_and_a_part_stays_within_its_window) {
  auto shared = made(ipc::SharedVmObject::create(4 * PAGE_SIZE));
  KREQUIRE(shared);
  KEXPECT_EQ(shared->pages(), 4u);

  memory::vmm::AddressSpace& kernel = *memory::vmm::kernel_space();
  const AccessFlags rw = AccessFlags::Read | AccessFlags::Write;
  const auto one = shared->map_into(kernel, CapRights::ReadWrite, rw);
  const auto two = shared->map_into(kernel, CapRights::ReadWrite, rw);
  KREQUIRE(one && two);

  for (std::uint64_t page = 0; page < 4; ++page) {
    (*one + static_cast<VirtualAddress::OffsetType>(page * PAGE_SIZE))
        .as<volatile std::uint64_t>()[0] = 0xF00 + page;
  }

  KEXPECT_EQ((*two + 2 * PAGE_SIZE).as<volatile std::uint64_t>()[0], 0xF02u);

  // A window onto pages 1 and 2.
  auto slice = made(shared->derive(CapRights::Mint, 1, 2));
  KREQUIRE(slice);
  const auto in_slice = slice->map_into(kernel, CapRights::Read, AccessFlags::Read);
  KREQUIRE(in_slice.has_value());
  KEXPECT_EQ(in_slice->as<volatile std::uint64_t>()[0], 0xF01u);
  KEXPECT_EQ((*in_slice + PAGE_SIZE).as<volatile std::uint64_t>()[0], 0xF02u);

  const auto page_one = shared->translate(CapRights::Read, 1);
  const auto slice_zero = slice->translate(CapRights::Read, 0);
  KEXPECT(page_one && slice_zero && *page_one == *slice_zero);

  // Out of the window, by any route: first pages, page counts, and sizes that would wrap.
  constexpr std::size_t HUGE = std::size_t{1} << 52;
  KEXPECT(slice->translate(CapRights::Read, 2).error() == Error::OutOfBounds);
  KEXPECT(slice->map_into(kernel, CapRights::Read, AccessFlags::Read, HUGE).error() ==
          Error::OutOfBounds);
  KEXPECT(slice->map_into(kernel, CapRights::Read, AccessFlags::Read, 1, HUGE).error() ==
          Error::OutOfBounds);
  KEXPECT(slice->derive(CapRights::Mint, HUGE, 1).error() == Error::OutOfBounds);
  KEXPECT(slice->derive(CapRights::Mint, 1, HUGE).error() == Error::OutOfBounds);
  KEXPECT(slice->derive(CapRights::Mint, 0, 3).error() == Error::OutOfBounds);
  KEXPECT(slice->resize(CapRights::All, 8).error() == Error::InvalidState);

  // And no more than the capability allows.
  KEXPECT(slice->map_into(kernel, CapRights::Read, rw).error() == Error::AccessDenied);
  KEXPECT(shared->map_into(kernel, CapRights::ReadWrite, AccessFlags::Read | AccessFlags::Execute)
              .error() == Error::AccessDenied);
  KEXPECT(shared->derive(CapRights::ReadWrite, 0, 1).error() == Error::AccessDenied);

  KEXPECT(shared->resize(CapRights::Write | CapRights::Manage, 6).has_value());
  KEXPECT_EQ(shared->pages(), 6u);
  KEXPECT(shared->translate(CapRights::Read, 5).has_value());

  KEXPECT(kernel.free(*one).has_value());
  KEXPECT(kernel.free(*two).has_value());
  KEXPECT(kernel.free(*in_slice).has_value());
}

// DMA buffers come out of a quota, stay where the device was told they are, and only the pool's
// own buffers can be freed through it.
KTEST(DmaPool, buffers_come_from_a_quota_and_stay_put) {
  auto pool = made(ipc::DmaPool::create(4 * PAGE_SIZE));
  KREQUIRE(pool);
  memory::vmm::AddressSpace& kernel = *memory::vmm::kernel_space();
  constexpr CapRights ALL = CapRights::ReadWrite | CapRights::Manage;

  std::array<ipc::DmaSegment, 3> segments{};
  KEXPECT(pool->allocate(kernel, CapRights::ReadWrite, PAGE_SIZE).error() == Error::AccessDenied);
  const auto buffer = pool->allocate(kernel, ALL, 3 * PAGE_SIZE, false,
                                     memory::vmm::CacheMode::WriteBack, segments);
  KREQUIRE(buffer.has_value());
  KEXPECT_EQ(pool->allocated(), 3 * PAGE_SIZE);
  KEXPECT(buffer->first_phys_addr == segments[0].phys_addr);

  for (std::size_t i = 0; i < segments.size(); ++i) {
    const VirtualAddress page =
        buffer->virt_addr + static_cast<VirtualAddress::OffsetType>(i * PAGE_SIZE);
    page.as<volatile std::uint64_t>()[0] = 0xD0 + i;
    const auto found = kernel.pagemap().translate(page);
    KEXPECT(found && found->phys_address == segments[i].phys_addr);
  }

  KEXPECT(pool->allocate(kernel, ALL, 2 * PAGE_SIZE).error() == Error::OutOfMemory);
  KEXPECT(pool->free(kernel, ALL, buffer->virt_addr + PAGE_SIZE).error() == Error::NotFound);
  KEXPECT(pool->free(kernel, CapRights::Read, buffer->virt_addr).error() == Error::AccessDenied);
  KEXPECT(pool->free(kernel, ALL, buffer->virt_addr).has_value());
  KEXPECT_EQ(pool->allocated(), 0u);

  const auto whole = pool->allocate(kernel, ALL, 4 * PAGE_SIZE, false);
  KREQUIRE(whole.has_value());
  pool.reset();
  settle();

  // The mapping holds the pages of its own: still there, until it goes.
  whole->virt_addr.as<volatile std::uint64_t>()[0] = 1;
  KEXPECT(kernel.free(whole->virt_addr).has_value());
}

// A kernel thread with a ThreadCap does IPC through its own context, and the cap knows when it is
// gone. A call it was to answer when it went fails instead of waiting forever.
KTEST(ThreadCap, follows_its_thread_and_fails_what_it_left_unanswered) {
  auto cspace = made(CSpace::create());
  auto endpoint = made(Endpoint::create());
  KREQUIRE(cspace && endpoint);

  struct Scene {
    Endpoint* endpoint;
    std::atomic<std::uint32_t> answered{0};
  } scene{.endpoint = endpoint.get()};

  // Answers one call, then takes another and leaves without answering it.
  auto thread = made(ipc::ThreadCap::create_kernel_thread(
      "server",
      [](void* arg) noexcept {
        auto* s = static_cast<Scene*>(arg);
        ipc::ThreadIpcContext* ctx = ipc::ThreadCap::current_context();
        KEXPECT(ctx != nullptr);

        Message request{};
        KEXPECT(s->endpoint->recv(*ctx, request, CapRights::Read).has_value());
        KEXPECT(Endpoint::reply(*ctx, Message{0xE1, request.regs[0] * 2}).has_value());
        s->answered.fetch_add(1, std::memory_order_release);
        KEXPECT(s->endpoint->recv(*ctx, request, CapRights::Read).has_value());
      },
      &scene, cspace.get()));
  KREQUIRE(thread);
  KEXPECT(thread->is_alive());
  KEXPECT(thread->read_stats(CapRights::Read).has_value());
  KEXPECT(thread->read_stats(CapRights::Write).error() == Error::AccessDenied);

  ipc::ThreadIpcContext ctx;
  Message answer{};
  KEXPECT(endpoint->call(ctx, Message{0xE0, 21}, answer, 0, CapRights::Write).has_value());
  KEXPECT_EQ(answer.regs[0], 42u);

  // This one the thread takes and leaves with: the answer never comes, and the call says so once
  // the thread is destroyed.
  const auto unanswered = endpoint->call(ctx, Message{0xE0, 1}, answer, 0, CapRights::Write);
  KEXPECT(!unanswered && unanswered.error() == Error::NotFound);

  KEXPECT(wait_for([&] { return !thread->is_alive(); }));
  KEXPECT(thread->read_stats(CapRights::Read).error() == Error::InvalidState);
  KEXPECT(thread->request_exit(CapRights::Write).error() == Error::InvalidState);
}

// A thread's faults go to its fault endpoint, as calls; the handler's answer is what the thread
// gets back.
KTEST(ThreadCap, faults_go_to_the_fault_endpoint) {
  auto handler_ep = made(Endpoint::create());
  KREQUIRE(handler_ep);

  struct Scene {
    ipc::ThreadCap* self{nullptr};
    std::atomic<bool> go{false};
    Error first{Error::InvalidState};
    Error second{Error::InvalidState};
    std::atomic<bool> done{false};
  };

  static Scene scene;
  scene.self = nullptr;
  scene.go.store(false);
  scene.first = Error::InvalidState;
  scene.second = Error::InvalidState;
  scene.done.store(false);

  auto thread = made(ipc::ThreadCap::create_kernel_thread(
      "faulty",
      [](void* arg) noexcept {
        auto* s = static_cast<Scene*>(arg);
        support::sleep_until_set(s->go);
        const auto first = s->self->send_fault(ipc::FaultMsgLabel::PageFault, 0x1000, 2, 0x4000);
        s->first = first ? Error::Success : first.error();
        const auto second = s->self->send_fault(ipc::FaultMsgLabel::UserException, 6, 0, 0x4004);
        s->second = second ? Error::Success : second.error();
        s->done.store(true, std::memory_order_release);
      },
      &scene, nullptr));
  KREQUIRE(thread);

  KEXPECT(thread->set_fault_endpoint(CapRights::Read, handler_ep.get(), 7).error() ==
          Error::AccessDenied);
  KEXPECT(thread->set_fault_endpoint(CapRights::Manage, handler_ep.get(), 0x77).has_value());
  scene.self = thread.get();
  scene.go.store(true, std::memory_order_release);

  ipc::ThreadIpcContext ctx;
  Message fault{};
  KEXPECT(handler_ep->recv(ctx, fault, CapRights::Read).has_value());
  KEXPECT_EQ(fault.tag.get_label(), std::to_underlying(ipc::FaultMsgLabel::PageFault));
  KEXPECT_EQ(fault.badge, 0x77u);
  KEXPECT_EQ(fault.regs[0], 0x1000u);
  KEXPECT_EQ(fault.regs[3], thread->tid());
  KEXPECT(Endpoint::reply(ctx, Message{0}).has_value());

  KEXPECT(handler_ep->recv(ctx, fault, CapRights::Read).has_value());
  KEXPECT_EQ(fault.tag.get_label(), std::to_underlying(ipc::FaultMsgLabel::UserException));
  Message refusal{0};
  refusal.tag.fail(Error::AccessDenied);
  KEXPECT(Endpoint::reply(ctx, refusal).has_value());

  KEXPECT(wait_for([&] { return scene.done.load(std::memory_order_acquire); }));
  KEXPECT(scene.first == Error::Success);
  KEXPECT(scene.second == Error::AccessDenied);
  KEXPECT(wait_for([&] { return !thread->is_alive(); }));
}

// A process as a capability: memory in it, shared objects and capabilities handed to it, and its
// end, after which the capability still holds it, Dead, with nothing left to do to it.
KTEST(ProcessCap, gives_its_holder_the_process_and_outlives_it_safely) {
  auto process = made(ipc::ProcessCap::create("capped"));
  KREQUIRE(process);
  const proc::ProcessId pid = process->pid();
  constexpr CapRights MANAGE = CapRights::Write | CapRights::Manage;
  const AccessFlags user_rw = AccessFlags::Read | AccessFlags::Write | AccessFlags::User;

  KEXPECT(process->allocate(CapRights::Write, PAGE_SIZE, user_rw).error() == Error::AccessDenied);
  const auto memory = process->allocate(MANAGE, 4 * PAGE_SIZE, user_rw);
  KREQUIRE(memory.has_value());
  KEXPECT(!memory->is_upper_half());
  KEXPECT(process->unmap(MANAGE, *memory).has_value());

  // A shared object the holder has a read-only capability to maps read-only, and no other way.
  auto mine = made(CSpace::create());
  auto shared = made(ipc::SharedVmObject::create(2 * PAGE_SIZE));
  KREQUIRE(mine && shared);
  const auto read_only = mine->insert(*shared, CapRights::Read);
  KREQUIRE(read_only.has_value());
  KEXPECT(process->map_shared(MANAGE, *mine, *read_only, user_rw).error() == Error::AccessDenied);
  const auto mapped =
      process->map_shared(MANAGE, *mine, *read_only, AccessFlags::Read | AccessFlags::User);
  KEXPECT(mapped.has_value());

  // Handing it a capability takes Grant, and gives no more than the mask.
  KEXPECT(process->grant(MANAGE, *mine, *read_only).error() == Error::AccessDenied);
  const auto granted = process->grant(CapRights::Grant, *mine, *read_only, CapRights::Read);
  KREQUIRE(granted.has_value());
  {
    const utils::rcu::ReadGuard rcu;
    const auto entry = process->cspace().lookup(*granted, rcu);
    KEXPECT(entry && entry->rights() == CapRights::Read && entry->object() == shared.get());
  }

  KEXPECT(process->terminate(CapRights::Write, 1).error() == Error::AccessDenied);
  KEXPECT(process->terminate(MANAGE, 5).has_value());
  int status = 0;
  KEXPECT(proc::kernel_process()->wait_child(pid, &status).has_value());
  KEXPECT_EQ(status, 5);

  // Collected, and still held: Dead, without an address space.
  KEXPECT(process->process().state() == proc::ProcessState::Dead);
  KEXPECT(process->allocate(MANAGE, PAGE_SIZE, user_rw).error() == Error::InvalidState);
  KEXPECT(process->unmap(MANAGE, *memory).error() == Error::InvalidState);
  process.reset();
  settle();
}
}  // namespace kernel::testing
