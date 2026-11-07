#include "hal/smp/ipi.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <memory>

#include "hal/interrupts/interrupt_manager.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "sched/scheduler.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::hw::ipi {
namespace {
constexpr std::uint8_t CALL_VECTOR = std::to_underlying(interrupts::InterruptVector::IpiCall);
constexpr std::uint8_t RESCHEDULE_VECTOR =
    std::to_underlying(interrupts::InterruptVector::IpiSchedule);

constexpr std::uint32_t INBOX_SLOTS = 128;  // Slots per inbox.
static_assert(std::has_single_bit(INBOX_SLOTS));

// Calls a CPU can have in flight with Wait::No before it has to wait for one to finish.
constexpr std::uint32_t ASYNC_REQUESTS = 16;

// Added to a request's count while it is being posted, so that targets that finish early cannot
// take it to zero before every target has it. Has to exceed any possible number of targets.
constexpr std::uint32_t POSTING_BIAS = 1u << 31;
static_assert(POSTING_BIAS > smp::MAX_CPUS);

struct alignas(std::hardware_destructive_interference_size) Request {
  Function fn{nullptr};
  void* arg{nullptr};
  std::atomic<std::uint32_t> pending{0};  // Targets that have not finished yet.
};

// One inbox slot. `seq` says whose turn it is: a sender may fill the slot when it equals the
// sender's ticket, the owner may take it when it equals ticket + 1 (Vyukov's bounded queue).
struct Slot {
  std::atomic<std::uint64_t> seq{0};
  Request* request{nullptr};
};

struct alignas(std::hardware_destructive_interference_size) Inbox {
  // Senders' side. The flag is set by whoever sends the IPI and cleared by the owner before it
  // looks at the slots, so a sender that finds it set knows an IPI is already on the way.
  std::atomic<std::uint64_t> tail{0};
  std::atomic<bool> kicked{false};

  // Owner's side.
  alignas(std::hardware_destructive_interference_size) std::uint64_t head{0};
  alignas(std::hardware_destructive_interference_size) std::array<Slot, INBOX_SLOTS> slots{};

  // Requests this CPU sends with Wait::No. They outlive the call, so they cannot live on the
  // sender's stack; a slot is free again once its count is back to zero.
  std::array<Request, ASYNC_REQUESTS> async{};
};

Inbox* g_inboxes = nullptr;

// Runs everything waiting in @p box. Owner only, with interrupts disabled.
void drain(Inbox& box) noexcept {
  while (true) {
    const std::uint64_t pos = box.head;
    Slot& slot = box.slots[pos % INBOX_SLOTS];

    // Either empty, or a sender has claimed the slot and is still filling it.
    if (slot.seq.load(std::memory_order_acquire) != pos + 1) {
      return;
    }

    Request* req = slot.request;

    // Give the slot back before running anything: the function may make calls of its own,
    // which may end up draining this inbox again from inside it.
    box.head = pos + 1;
    slot.seq.store(pos + INBOX_SLOTS, std::memory_order_release);

    req->fn(req->arg);

    // The request may be gone the moment this lands.
    req->pending.fetch_sub(1, std::memory_order_release);
  }
}

void drain_local() noexcept { drain(g_inboxes[percpu::id()]); }

// Puts @p req into @p target's inbox. Lock-free for any number of senders.
void post(const std::uint32_t target, Request* req) noexcept {
  Inbox& box = g_inboxes[target];
  std::uint64_t pos = box.tail.load(std::memory_order_relaxed);

  while (true) {
    Slot& slot = box.slots[pos % INBOX_SLOTS];
    const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
    const auto lag = static_cast<std::int64_t>(seq - pos);

    if (lag == 0) {
      if (box.tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        slot.request = req;
        slot.seq.store(pos + 1, std::memory_order_release);
        return;
      }
    } else if (lag < 0) {
      // Full. Its owner may be busy waiting for us, so answer our own calls while we wait.
      drain_local();
      cpu_relax();
      pos = box.tail.load(std::memory_order_relaxed);
    } else {
      // Another sender took this slot first.
      pos = box.tail.load(std::memory_order_relaxed);
    }
  }
}

/// Whether @p target needs an IPI for what was just posted to it.
[[nodiscard]] bool needs_kick(const std::uint32_t target) noexcept {
  return !g_inboxes[target].kicked.exchange(true, std::memory_order_acq_rel);
}

class Kicker {
  const apic::Lapic& m_lapic;
  const bool m_clustered;
  std::uint32_t m_cluster{~0u};
  std::uint32_t m_members{0};

 public:
  Kicker() noexcept : m_lapic(percpu::lapic()), m_clustered(smp::uses_cluster_destinations()) {}

  Kicker(const Kicker&) = delete;
  Kicker& operator=(const Kicker&) = delete;

  void add(const std::uint32_t cpu) noexcept {
    const smp::CpuEntry& e = smp::entry(cpu);

    // A zero logical ID would address nobody; some hypervisors leave the LDR empty.
    if (!m_clustered || e.logical_dest == 0) [[unlikely]] {
      m_lapic.send_ipi(e.apic_id, CALL_VECTOR);
      return;
    }

    const std::uint32_t cluster = e.logical_dest >> 16;
    if (cluster != m_cluster) {
      flush();
      m_cluster = cluster;
    }

    m_members |= e.logical_dest & 0xFFFF;
  }

  void flush() noexcept {
    if (m_members != 0) {
      m_lapic.send_ipi((m_cluster << 16) | m_members, CALL_VECTOR, apic::DeliveryMode::Fixed,
                       apic::DestinationMode::Logical);
      m_members = 0;
    }
  }
};

// A free request for a Wait::No call. Interrupts are disabled, so nothing else on this CPU can
// claim the same one.
[[nodiscard]] Request* claim_async(const std::uint32_t me) noexcept {
  auto& pool = g_inboxes[me].async;

  while (true) {
    for (Request& req : pool) {
      if (req.pending.load(std::memory_order_acquire) == 0) {
        return &req;
      }
    }

    // All in flight. What their targets are waiting for may be sitting in our own inbox.
    drain_local();
    cpu_relax();
  }
}

void wait_for(const Request& req) noexcept {
  while (req.pending.load(std::memory_order_acquire) != 0) {
    poll();
    cpu_relax();
  }
}

std::uint32_t dispatch(const Selector select, const void* ctx, const Function fn, void* arg,
                       const Wait wait, const bool include_self) noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (!g_inboxes || smp::online_cpus() <= 1) [[unlikely]] {
    if (include_self) {
      utils::IrqDisableGuard guard;
      fn(arg);
    }
    return 0;
  }

  Request local;
  Request* req = nullptr;
  std::uint32_t posted = 0;

  {
    utils::IrqDisableGuard guard;
    const std::uint32_t me = percpu::id();

    req = wait == Wait::Yes ? &local : claim_async(me);
    req->fn = fn;
    req->arg = arg;
    req->pending.store(POSTING_BIAS, std::memory_order_relaxed);

    // With every CPU online and every one of them targeted, a single "all but self" IPI does
    // the job of one per CPU. The per-target flags can be skipped then: the IPI is sent after
    // all the posts, and reaches everybody regardless.
    const bool broadcast = select == nullptr && smp::all_online();
    Kicker kicker;

    smp::for_each_online([&](const std::uint32_t cpu) {
      if (cpu == me || (select && !select(cpu, ctx))) {
        return;
      }

      post(cpu, req);
      ++posted;

      if (!broadcast && needs_kick(cpu)) {
        kicker.add(cpu);
      }
    });

    if (broadcast && posted != 0) {
      percpu::lapic().send_ipi(apic::DestinationShorthand::AllExcludingSelf, CALL_VECTOR);
    } else {
      kicker.flush();
    }

    // Drop the bias. Whatever is left is the targets that have not finished yet.
    req->pending.fetch_sub(POSTING_BIAS - posted, std::memory_order_acq_rel);

    // Our own share runs while the others run theirs.
    if (include_self) {
      fn(arg);
    }
  }

  if (wait == Wait::Yes) {
    wait_for(*req);
  }

  return posted;
}

interrupts::InterruptStatus on_call(const interrupts::Event&, void*) noexcept {
  Inbox& box = g_inboxes[percpu::id()];

  // Clear the flag before looking at the slots: anyone posting from now on sees it clear and
  // sends another IPI, so nothing can slip in behind the drain.
  box.kicked.exchange(false, std::memory_order_acq_rel);
  drain(box);
  return interrupts::InterruptStatus::Handled;
}

// Waking the CPU up is most of the point. If it was busy, the scheduler takes it from there.
interrupts::InterruptStatus on_reschedule(const interrupts::Event&, void*) noexcept {
  sched::Scheduler::tick();
  return interrupts::InterruptStatus::Handled;
}

constinit interrupts::InterruptRegistration g_call_handler{&on_call, nullptr};
constinit interrupts::InterruptRegistration g_reschedule_handler{&on_reschedule, nullptr};
}  // namespace

std::expected<void, Error> call(const std::uint32_t cpu, const Function fn, void* arg,
                                const Wait wait) noexcept {
  if (cpu >= smp::possible_cpus()) [[unlikely]] {
    return std::unexpected(Error::InvalidArgument);
  }

  Request local;
  Request* req = nullptr;

  {
    utils::IrqDisableGuard guard;
    const std::uint32_t me = percpu::id();

    if (cpu == me) {
      fn(arg);
      return {};
    }

    if (!g_inboxes || !smp::is_online(cpu)) {
      return std::unexpected(Error::InvalidState);
    }

    req = wait == Wait::Yes ? &local : claim_async(me);
    req->fn = fn;
    req->arg = arg;
    req->pending.store(1, std::memory_order_relaxed);

    post(cpu, req);
    if (needs_kick(cpu)) {
      percpu::lapic().send_ipi(smp::entry(cpu).apic_id, CALL_VECTOR);
    }
  }

  if (wait == Wait::Yes) {
    wait_for(*req);
  }

  return {};
}

std::uint32_t call_many(const Selector select, const void* ctx, const Function fn, void* arg,
                        const Wait wait) noexcept {
  return dispatch(select, ctx, fn, arg, wait, false);
}

void call_all(const Function fn, void* arg, const Wait wait) noexcept {
  (void)dispatch(nullptr, nullptr, fn, arg, wait, true);
}

void send_reschedule(const std::uint32_t cpu) noexcept {
  if (!smp::is_online(cpu)) {
    return;
  }

  utils::IrqDisableGuard guard;
  const apic::Lapic& lapic = percpu::lapic();

  if (cpu == percpu::id()) {
    lapic.send_self_ipi(RESCHEDULE_VECTOR);
  } else {
    lapic.send_ipi(smp::entry(cpu).apic_id, RESCHEDULE_VECTOR);
  }
}

void poll() noexcept {
  if (!g_inboxes) [[unlikely]] {
    return;
  }

  utils::IrqDisableGuard guard;
  drain_local();
}

void initialize() noexcept {
  const std::uint32_t cpus = smp::possible_cpus();

  auto* boxes = static_cast<Inbox*>(memory::heap::kmalloc(sizeof(Inbox) * cpus, alignof(Inbox)));
  if (!boxes) {
    utils::logger::fatal("IPI: out of memory for {} inboxes\n", cpus);
  }

  std::uninitialized_default_construct_n(boxes, cpus);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    for (std::uint32_t i = 0; i < INBOX_SLOTS; ++i) {
      boxes[cpu].slots[i].seq.store(i, std::memory_order_relaxed);
    }
  }

  // The APs are started after this, which publishes the inboxes to them.
  g_inboxes = boxes;

  interrupts::InterruptManager::register_handler(CALL_VECTOR, &g_call_handler);
  interrupts::InterruptManager::register_handler(RESCHEDULE_VECTOR, &g_reschedule_handler);
}
}  // namespace kernel::hw::ipi
