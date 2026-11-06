#include "sched/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono.hpp>
#include <span>
#include <tuple>
#include <utility>

#include "drivers/clock.hpp"
#include "hal/smp/cpu_info.hpp"
#include "hal/smp/ipi.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "memory/heap.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "utils/lock.hpp"
#include "utils/logger.hpp"

namespace kernel::sched {
namespace {
namespace smp = hw::smp;

// How many overloaded CPUs an idle one tries before it gives up and sleeps.
constexpr std::uint32_t MAX_STEAL_ATTEMPTS = 8;

// What a queue on another node must have to spare, by RunQueue::spare(), before its threads are
// worth moving away from their memory.
constexpr std::uint32_t REMOTE_STEAL_THRESHOLD = 2;

constinit std::atomic<bool> g_initialized{false};

[[nodiscard]] std::uint64_t now_ns() noexcept { return drivers::SystemClock::now().raw_ns(); }

hw::timer::TimerAction on_slice_timer(hw::timer::Timer&, void*) noexcept {
  Scheduler::tick();
  return hw::timer::TimerAction::Done;
}

// Keeps the slice timer ahead of the running thread. If it goes off earlier than it has to, it
// finds out what is left of the slice then, and that beats starting a timer on every switch.
void arm_slice_timer(RunQueue& rq, const std::uint64_t now) noexcept {
  if (!rq.wants_timer_locked()) {
    return;
  }

  hw::timer::Timer& timer = rq.slice_timer();
  const klib::TimePoint expiry{now + std::max(rq.slice_left_ns_locked(), MIN_SLICE_NS)};

  if (!timer.is_pending() || expiry < timer.deadline()) {
    (void)timer.start_at(expiry);
  }
}

// The number of low APIC ID bits that tell the SMT siblings of a core apart.
[[nodiscard]] std::uint32_t smt_id_bits(const hw::CpuInfo& info) noexcept {
  for (const hw::TopologyInfo& level : info.topology()) {
    if (level.level_type == hw::TopoLevel::SMT) {
      return level.id_shift;
    }
  }

  return 0;
}

struct CpuRecord {
  std::uint32_t node;
  std::uint32_t core;
  std::uint32_t cpu;
};

// Every node's domain, in slots sorted by core, so that siblings end up next to each other.
void build_domains() noexcept {
  const std::uint32_t cpus = smp::possible_cpus();
  const std::uint32_t nodes = smp::node_count();
  const std::uint32_t smt_bits = smt_id_bits(*hw::percpu::self()->info);

  auto* records = static_cast<CpuRecord*>(memory::heap::kmalloc(sizeof(CpuRecord) * cpus));
  auto* placements = static_cast<CpuPlacement*>(memory::heap::kmalloc(sizeof(CpuPlacement) * cpus));
  auto* neighbors =
      static_cast<std::uint32_t*>(memory::heap::kmalloc(sizeof(std::uint32_t) * nodes));
  auto* node_cpus =
      static_cast<std::uint32_t*>(memory::heap::kmalloc(sizeof(std::uint32_t) * nodes));
  if (!records || !placements || !neighbors || !node_cpus) {
    utils::logger::fatal("sched: out of memory for the topology of {} CPUs\n", cpus);
  }

  std::fill_n(node_cpus, nodes, 0u);
  for (std::uint32_t cpu = 0; cpu < cpus; ++cpu) {
    const smp::CpuEntry& entry = smp::entry(cpu);
    records[cpu] = {.node = entry.numa_node, .core = entry.apic_id >> smt_bits, .cpu = cpu};
    ++node_cpus[entry.numa_node];
  }

  const std::span sorted{records, cpus};
  std::ranges::sort(sorted, [](const CpuRecord& a, const CpuRecord& b) {
    return std::tie(a.node, a.core, a.cpu) < std::tie(b.node, b.core, b.cpu);
  });

  std::uint32_t next_record = 0;
  for (std::uint32_t node = 0; node < nodes; ++node) {
    const std::uint32_t count = node_cpus[node];
    for (std::uint32_t slot = 0; slot < count; ++slot) {
      const CpuRecord& record = records[next_record++];
      placements[slot] = {.cpu = record.cpu, .core = record.core};
      smp::entry(record.cpu).node_slot = slot;
    }

    // SLIT distances from here to every other node that has a CPU to run on.
    std::uint32_t neighbor_count = 0;
    for (std::uint32_t other = 0; other < nodes; ++other) {
      if (other != node && node_cpus[other] != 0) {
        neighbors[neighbor_count++] = other;
      }
    }

    const std::span nearby{neighbors, neighbor_count};
    std::ranges::sort(nearby, {}, [node](const std::uint32_t other) {
      return memory::pmm::g_topology.get_latency(node, other);
    });

    const auto built =
        smp::node_area(node)->scheduler.initialize(node, {placements, count}, nearby);
    if (!built) {
      utils::logger::fatal("sched: out of memory for the domain of node {}\n", node);
    }
  }

  memory::heap::kfree(node_cpus);
  memory::heap::kfree(neighbors);
  memory::heap::kfree(placements);
  memory::heap::kfree(records);
}

[[nodiscard]] std::uint32_t claim_in(NumaDomain& domain) noexcept {
  const std::uint32_t cpu = domain.claim_idle_core();
  return cpu != NumaDomain::NO_CPU ? cpu : domain.claim_idle_cpu();
}

// The less busy of two samples of @p home, where @p favorite gets to be one of them if it can.
[[nodiscard]] std::uint32_t less_busy_cpu(const NumaDomain& home,
                                          const std::uint32_t favorite) noexcept {
  RunQueue& self = hw::percpu::runqueue();

  if (home.cpu_count() == 0) {
    return smp::is_online(favorite) ? favorite : self.cpu_id();
  }

  const auto sample = [&] { return home.cpu_at(self.next_random() % home.cpu_count()); };

  const std::uint32_t first = smp::is_online(favorite) ? favorite : sample();
  const std::uint32_t second = sample();

  if (!smp::is_online(first)) {
    return smp::is_online(second) ? second : self.cpu_id();
  }

  if (!smp::is_online(second)) {
    return first;
  }

  return Scheduler::cpu_runqueue(second).load() < Scheduler::cpu_runqueue(first).load() ? second
                                                                                        : first;
}

// Wakes an idle CPU to come and take a thread that is waiting on @p rq, if there is such a CPU. A
// CPU only goes looking for work on its way to sleep, so one that is asleep already has to be told.
// That goes for every way a queue can grow: a new thread, one that lost the CPU to a real-time
// thread, or one that picked its CPU just before another one went idle.
void offer_spare_work(RunQueue& rq) noexcept {
  const std::uint32_t spare = rq.spare();
  NumaDomain* home = rq.domain();
  if (spare == 0 || home == nullptr) {
    return;
  }

  std::uint32_t helper = home->claim_idle_cpu();

  if (helper == NumaDomain::NO_CPU && spare >= REMOTE_STEAL_THRESHOLD) {
    for (const std::uint32_t node : home->neighbors()) {
      helper = Scheduler::numa_domain(node).claim_idle_cpu();
      if (helper != NumaDomain::NO_CPU) {
        break;
      }
    }
  }

  if (helper != NumaDomain::NO_CPU) {
    hw::ipi::send_reschedule(helper);
  }
}

// Puts a thread nobody else is touching on the queue of the best CPU, and gets that CPU's
// attention if it needs it. @return false if there is no CPU for it to go to.
[[nodiscard]] bool place(Thread* thread) noexcept {
  utils::IrqDisableGuard irq_guard;

  const std::uint32_t target = Scheduler::select_target_cpu(thread);
  if (!smp::is_online(target)) {
    return false;
  }

  thread->set_cpu_id(target);

  RunQueue& rq = Scheduler::cpu_runqueue(target);
  const bool local = target == hw::percpu::id();
  const std::uint64_t now = now_ns();
  bool wants_attention = false;

  rq.lock();
  rq.update_clock_locked(now);
  const bool preempts = rq.enqueue_locked(*thread);

  if (local) {
    if (preempts) {
      hw::percpu::request_reschedule();
    }

    arm_slice_timer(rq, now);
  } else {
    // The first thread to wait also means that the CPU has to start its slice timer.
    wants_attention = preempts || rq.waiting() == 1;
  }

  const bool target_busy = rq.current() != rq.idle_thread();
  rq.unlock();

  if (wants_attention) {
    hw::ipi::send_reschedule(target);
  }

  if (target_busy) {
    offer_spare_work(rq);
  }

  return true;
}

// A wake or enqueue may have left the calling thread with a reason to get out of the way.
void reschedule_if_needed() noexcept {
  if (!hw::percpu::preempt_pending() || !hw::irq::is_enabled()) {
    return;
  }

  const Thread* self = current();
  if (self != nullptr && !self->is_idle() && self == hw::percpu::runqueue().current()) {
    Scheduler::schedule();
  }
}

// Tries the overloaded CPUs of @p domain, as long as @p attempts lasts.
[[nodiscard]] Thread* steal_from(const NumaDomain& domain, const std::uint32_t start_slot,
                                 const std::uint32_t enough, const std::uint32_t self,
                                 std::uint32_t& attempts) noexcept {
  Thread* stolen = nullptr;

  domain.for_each_overloaded(start_slot, [&](const std::uint32_t cpu) {
    if (cpu == self || !smp::is_online(cpu)) {
      return false;
    }

    stolen = Scheduler::cpu_runqueue(cpu).try_steal(enough);
    return stolen != nullptr || --attempts == 0;
  });

  return stolen;
}

// Moves a thread from a CPU that has one to spare to @p rq, nearest first.
[[nodiscard]] bool steal_work(RunQueue& rq) noexcept {
  utils::IrqDisableGuard irq_guard;

  if (rq.domain() == nullptr) {
    return false;
  }

  std::uint32_t attempts = MAX_STEAL_ATTEMPTS;
  const NumaDomain& home = *rq.domain();
  Thread* stolen = steal_from(home, rq.slot(), 1, rq.cpu_id(), attempts);

  for (const std::uint32_t node : home.neighbors()) {
    if (stolen != nullptr || attempts == 0) {
      break;
    }

    stolen =
        steal_from(Scheduler::numa_domain(node), 0, REMOTE_STEAL_THRESHOLD, rq.cpu_id(), attempts);
  }

  if (stolen == nullptr) {
    return false;
  }

  stolen->set_cpu_id(rq.cpu_id());

  rq.lock();
  rq.update_clock_locked(now_ns());
  (void)rq.enqueue_locked(*stolen);
  rq.unlock();
  return true;
}

// Frees what is left of finished threads, if this is a context where that may wait for others.
void reap_if_possible() noexcept {
  if (hw::percpu::can_sleep() && hw::irq::is_enabled()) {
    (void)hw::percpu::runqueue().drain_dead();
  }
}
}  // namespace

void Scheduler::initialize() noexcept {
  build_domains();
  initialize_cpu();
  g_initialized.store(true, std::memory_order_release);
}

void Scheduler::initialize_cpu() noexcept {
  const std::uint32_t cpu = hw::percpu::id();
  const smp::CpuEntry& entry = smp::entry(cpu);

  KASSERT(current() != nullptr);

  RunQueue& rq = hw::percpu::runqueue();
  rq.initialize(cpu, &numa_domain(entry.numa_node), entry.node_slot, current());
  rq.slice_timer().set_callback(&on_slice_timer);
}

bool Scheduler::is_initialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

RunQueue& Scheduler::cpu_runqueue(const std::uint32_t cpu_id) noexcept {
  return hw::percpu::cpu_area(cpu_id)->runqueue;
}

NumaDomain& Scheduler::numa_domain(const std::uint32_t node_id) noexcept {
  return smp::node_area(node_id)->scheduler;
}

std::uint32_t Scheduler::select_target_cpu(const Thread* thread) noexcept {
  const std::uint32_t last = thread->cpu_id();
  if (thread->is_pinned()) {
    return last;
  }

  const bool last_online = smp::is_online(last);

  std::uint32_t node = thread->preferred_numa_node();
  if (node >= smp::node_count()) {
    node = last_online ? smp::entry(last).numa_node : hw::percpu::numa_node_id();
  }

  NumaDomain& home = numa_domain(node);

  const bool last_is_home = last_online && smp::entry(last).numa_node == node;
  if (last_is_home && home.claim_cpu(smp::entry(last).node_slot)) {
    return last;
  }

  if (const std::uint32_t cpu = claim_in(home); cpu != NumaDomain::NO_CPU) {
    return cpu;
  }

  for (const std::uint32_t neighbor : home.neighbors()) {
    if (const std::uint32_t cpu = claim_in(numa_domain(neighbor)); cpu != NumaDomain::NO_CPU) {
      return cpu;
    }
  }

  return less_busy_cpu(home, last_is_home ? last : smp::INVALID_CPU);
}

auto Scheduler::enqueue(Thread* thread) noexcept -> std::expected<void, Error> {
  if (thread == nullptr || thread->is_idle()) {
    return std::unexpected(Error::InvalidArgument);
  }

  if (!is_initialized()) {
    return std::unexpected(Error::InvalidState);
  }

  const ThreadState state = thread->state();
  if ((state != ThreadState::Ready && state != ThreadState::Created) ||
      !thread->try_hand_to_scheduler()) {
    return std::unexpected(Error::InvalidState);
  }

  if (!place(thread)) {
    thread->revoke_from_scheduler();
    return std::unexpected(Error::InvalidState);
  }

  reschedule_if_needed();
  return {};
}

bool Scheduler::wake(Thread* thread) noexcept {
  {
    utils::IrqDisableGuard irq_guard;

    // The queue of the CPU it was last on is the one that knows whether it has really left: that
    // lock is held all the way through a context switch.
    while (true) {
      const std::uint32_t cpu = thread->cpu_id();
      RunQueue& rq = cpu_runqueue(cpu);
      utils::NakedGuard guard{rq};

      if (thread->cpu_id() != cpu) {
        continue;  // Woken by someone else, and run elsewhere since we looked. Look again.
      }

      const ThreadState state = thread->state();
      if (state != ThreadState::Blocked && state != ThreadState::Sleeping) {
        return false;
      }

      if (rq.current() == thread) {
        // It announced the block but has not got round to it. schedule() will see this.
        thread->set_state(ThreadState::Running);
        return true;
      }

      thread->mark_queued();
      break;
    }

    // Other wakers go away now that it is Ready, and nothing else knows about it yet.
    if (!place(thread)) {
      // Pinned to a CPU that went away. Nothing does that yet; when something does, the thread
      // needs a new home here.
      utils::logger::fatal("sched: thread {} has no CPU to run on\n", thread->tid());
    }
  }

  reschedule_if_needed();
  return true;
}

void Scheduler::prepare_to_block() noexcept { current()->set_state(ThreadState::Blocked); }

void Scheduler::cancel_block() noexcept { current()->set_state(ThreadState::Running); }

void Scheduler::switch_away(const PickMode mode) noexcept {
  const std::uint64_t flags = hw::irq::save();
  hw::percpu::preempt_disable();

  RunQueue& rq = hw::percpu::runqueue();
  Thread* prev = current();

  rq.lock();
  hw::percpu::clear_reschedule();

  const std::uint64_t now = now_ns();
  rq.update_clock_locked(now);

  Thread* next = rq.pick_next_locked(*prev, mode);
  arm_slice_timer(rq, now);

  if (next == prev) {
    rq.unlock();
    hw::percpu::preempt_enable();
  } else {
    rq.hand_over_lock();
    switch_to(prev, next,
              mode == PickMode::Preempted ? SwitchReason::Preempted : SwitchReason::Voluntary);

    // Some time later, on some CPU, this thread is back. The rq from above may not be ours now.
    finish_switch();
  }

  hw::irq::restore(flags);
}

void Scheduler::finish_switch() noexcept {
  RunQueue& rq = hw::percpu::runqueue();
  if (rq.take_lock_handover()) {
    rq.unlock();
    hw::percpu::preempt_enable();

    // The thread that just lost this CPU may have gone back on the queue.
    offer_spare_work(rq);
  }
}

void Scheduler::schedule() noexcept {
  switch_away(PickMode::Voluntary);
  reap_if_possible();
}

void Scheduler::yield() noexcept {
  switch_away(PickMode::Yield);
  reap_if_possible();
}

void Scheduler::preempt_from_interrupt() noexcept {
  // Threads run by hand are none of our business, and neither is one on its way out: it is about
  // to leave anyway, and putting it back on a queue would take away the fact that it is dead.
  const Thread* self = current();
  if (self == nullptr || self->is_idle() || self != hw::percpu::runqueue().current() ||
      self->state() == ThreadState::Dead) {
    return;
  }

  switch_away(PickMode::Preempted);
}

void Scheduler::tick() noexcept {
  RunQueue& rq = hw::percpu::runqueue();
  if (!rq.is_ready()) {
    return;
  }

  const std::uint64_t now = now_ns();
  {
    utils::NakedGuard guard{rq};

    rq.update_clock_locked(now);
    if (rq.should_switch_locked()) {
      hw::percpu::request_reschedule();
    }

    arm_slice_timer(rq, now);
  }

  offer_spare_work(rq);
}

[[noreturn]] void Scheduler::exit_current() noexcept {
  Thread* self = current();

  // Interrupts stay off from here on: this stack is not to be used for anything else now, and
  // the next thing to touch it is whoever destroys it.
  hw::irq::disable();

  if (self != hw::percpu::runqueue().current()) {
    // Run by hand, and its exit hook came back. Whoever did that has to deal with it.
    hw::irq::enable();
    while (true) {
      asm volatile("hlt");
    }
  }

  self->set_state(ThreadState::Dead);
  switch_away(PickMode::Voluntary);
  std::unreachable();
}

void Scheduler::idle_step() noexcept {
  RunQueue& rq = hw::percpu::runqueue();
  (void)rq.drain_dead();

  if (rq.waiting() == 0) {
    // In this order. Whoever queues a thread reads the idle bitmaps after doing so, and this reads
    // the queue after setting its bit, so a thread that arrives right now is seen by one of us.
    rq.publish_idle();

    if (rq.waiting() == 0 && !steal_work(rq)) {
      hw::cpu_idle(rq.idle_word());
      return;
    }
  }

  schedule();
}

[[noreturn]] void Scheduler::enter_idle_loop() noexcept {
  while (true) {
    idle_step();
  }
}
}  // namespace kernel::sched
