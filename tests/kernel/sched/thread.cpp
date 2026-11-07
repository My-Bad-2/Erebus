#include "sched/thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>

#include "../hal/fpu_registers.hpp"
#include "hal/hal.hpp"
#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "ktest.hpp"
#include "memory/memory.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "memory/shrinker.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "sched/fpu_guard.hpp"
#include "utils/linked_list.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
namespace fpu = hw::fpu;
using memory::VirtualAddress;
using sched::MIN_KSTACK_SIZE;
using sched::SwitchContext;
using sched::Thread;
using sched::ThreadCreateOptions;
using sched::ThreadFlags;
using sched::ThreadState;

constexpr std::uint32_t TRACKED_CPUS = 1024;

void do_nothing(void*) noexcept {}

// Exit hook: what a thread that finished can do without a scheduler, go back to who started it.
void return_to_parent(Thread* self, void* parent) noexcept {
  sched::switch_to(self, static_cast<Thread*>(parent));
}

// Runs @p thread on this CPU until its entry function has returned, and cleans up after it.
void run_to_completion(Thread* thread) noexcept {
  Thread* self = sched::current();
  thread->set_exit_hook(&return_to_parent, self);
  sched::switch_to(self, thread);

  KEXPECT(thread->state() == ThreadState::Dead);
  KEXPECT(Thread::destroy(thread).has_value());
}

[[nodiscard]] std::uint64_t stack_pointer() noexcept {
  std::uint64_t rsp;
  asm volatile("movq %%rsp, %0" : "=r"(rsp));
  return rsp;
}

[[nodiscard]] std::uint32_t read_pkru() noexcept {
  std::uint32_t pkru;
  asm volatile(
      "xorl %%ecx, %%ecx\n\t"
      "rdpkru"
      : "=a"(pkru)
      : /* No Input */
      : "ecx", "edx");
  return pkru;
}

// What a new thread finds when it first runs.
struct EntryState {
  Thread* self{nullptr};
  std::uint8_t preempt_count{0xFF};
  bool interrupts_on{false};
  bool can_sleep{false};
  std::uint32_t cpu{~0u};
  std::uint64_t rsp{0};
};

void record_entry_state(void* arg) noexcept {
  auto* state = static_cast<EntryState*>(arg);
  state->self = sched::current();
  state->preempt_count = hw::percpu::preempt_count();
  state->interrupts_on = hw::irq::is_enabled();
  state->can_sleep = hw::percpu::can_sleep();
  state->cpu = hw::percpu::id();
  state->rsp = stack_pointer();
}

// Called like any function, so this is the stack pointer the ABI makes promises about.
[[gnu::naked]] void record_stack_pointer(void*) noexcept {
  asm("movq %rsp, (%rdi)\n\t"
      "retq\n\t");
}

// A thread and its parent, taking turns.
struct PingPong {
  Thread* parent{nullptr};
  Thread* worker{nullptr};
  std::uint32_t rounds{0};
  std::uint32_t turns{0};
};

void ping_pong_worker(void* arg) noexcept {
  auto* game = static_cast<PingPong*>(arg);
  for (std::uint32_t round = 0; round < game->rounds; ++round) {
    ++game->turns;
    sched::switch_to(game->worker, game->parent);
  }
}

void record_fs_base(void* arg) noexcept { *static_cast<std::uint64_t*>(arg) = hw::read::fs_base(); }

void record_pkru(void* arg) noexcept { *static_cast<std::uint32_t*>(arg) = read_pkru(); }

struct Payload {
  std::uint32_t value;
};

void double_payload(Payload* payload) noexcept { payload->value *= 2; }

std::atomic<std::uint32_t> g_lambda_runs{0};

// Two threads taking turns with the same FPU registers, each expecting to find its own in them.
struct FpuWorker {
  Thread* self{nullptr};
  Thread* other{nullptr};
  std::uint64_t seed{0};
  std::uint32_t rounds{0};
  std::uint32_t mismatches{0};
};

void fpu_worker(void* arg) noexcept {
  auto* me = static_cast<FpuWorker*>(arg);
  const RegisterFile mine = make_pattern(me->seed);
  utils::NullLock lock;

  {
    utils::FpuGuard guard{lock};
    load_registers(mine);
  }

  for (std::uint32_t round = 0; round < me->rounds; ++round) {
    sched::switch_to(me->self, me->other);

    RegisterFile found{};
    {
      utils::FpuGuard guard{lock};
      store_registers(found);
    }

    if (!(found == mine)) {
      ++me->mismatches;
    }
  }
}

// Palette 1, with tile 0 at 16 rows of 64 bytes and the rest of the tiles left out.
using TileConfig = std::array<std::uint8_t, 64>;

constexpr TileConfig TILE_CONFIG = [] {
  TileConfig config{};
  config[0] = 1;    // Palette.
  config[16] = 64;  // Bytes per row of tile 0, the low half of a 16-bit field.
  config[48] = 16;  // Rows of tile 0.
  return config;
}();

void load_tile_config(const TileConfig& config) noexcept {
  asm volatile("ldtilecfg %0" : /* No Output */ : "m"(config) : "memory");
}

struct TileWorker {
  Thread* parent{nullptr};
  Thread* self{nullptr};
  bool loads_config{false};
};

// Takes two turns. Each ends with a section, so that the switch after it saves the registers.
void tile_worker(void* arg) noexcept {
  auto* me = static_cast<TileWorker*>(arg);
  utils::NullLock lock;

  {
    utils::FpuGuard guard{lock};
    if (me->loads_config) {
      load_tile_config(TILE_CONFIG);
    }
  }

  sched::switch_to(me->self, me->parent);

  {
    utils::FpuGuard guard{lock};
  }

  sched::switch_to(me->self, me->parent);
}

// Whether the last save for @p thread has TILE_CONFIG in it. Being the highest component a thread
// without AMX has, it comes last in the area in either format.
[[nodiscard]] bool has_saved_tile_config(Thread* thread) noexcept {
  const auto* area = static_cast<const std::byte*>(thread->arch_state().fpu_storage);
  const auto* header = reinterpret_cast<const fpu::XSaveHeader*>(area + fpu::XSAVE_HEADER_OFFSET);
  if ((header->xstate_bv & hw::XCR0::XTILECFG) == 0) {
    return false;
  }

  return klib::memcmp(area + fpu::area_size() - sizeof(TILE_CONFIG), TILE_CONFIG.data(),
                      sizeof(TILE_CONFIG)) == 0;
}

// Runs two FpuWorkers against each other on this CPU, and returns how often one of them found
// someone else's registers.
[[nodiscard]] std::uint32_t play_fpu_workers(const std::uint64_t seed,
                                             const std::uint32_t rounds) noexcept {
  FpuWorker one{.seed = seed, .rounds = rounds};
  FpuWorker two{.seed = seed + 1, .rounds = rounds};

  const auto first = Thread::create_kernel("fpu-one", &fpu_worker, &one);
  const auto second = Thread::create_kernel("fpu-two", &fpu_worker, &two);
  if (!first || !second) {
    KEXPECT(first.has_value());
    KEXPECT(second.has_value());
    return 0;
  }

  one.self = two.other = *first;
  two.self = one.other = *second;

  // `one` goes first and finishes first, right after handing `two` its last turn.
  run_to_completion(*first);
  run_to_completion(*second);
  return one.mismatches + two.mismatches;
}

// One thread visiting every CPU in turn, FPU registers and all.
Thread* g_traveller = nullptr;
Thread* g_homes[TRACKED_CPUS];
std::atomic<std::uint32_t> g_visitor_turn{0};
std::atomic<std::uint32_t> g_visits{0};
std::atomic<std::uint32_t> g_visit_mismatches{0};
std::atomic<std::uint32_t> g_wrong_cpu{0};
std::atomic<bool> g_journey_over{false};

void traveller(void*) noexcept {
  utils::NullLock lock;

  {
    utils::FpuGuard guard{lock};
    load_registers(make_pattern(1));
  }

  for (std::uint64_t leg = 1; !g_journey_over.load(std::memory_order_acquire); ++leg) {
    const std::uint32_t cpu = hw::percpu::id();
    if (sched::current()->cpu_id() != cpu) {
      g_wrong_cpu.fetch_add(1, std::memory_order_relaxed);
    }

    RegisterFile found{};
    {
      utils::FpuGuard guard{lock};
      store_registers(found);

      // New values for every leg, so what the next CPU finds can only have come from this one.
      load_registers(make_pattern(leg + 1));
    }

    if (!(found == make_pattern(leg))) {
      g_visit_mismatches.fetch_add(1, std::memory_order_relaxed);
    }

    g_visits.fetch_add(1, std::memory_order_relaxed);
    sched::switch_to(sched::current(), g_homes[cpu]);
  }
}
}  // namespace

static_assert(ThreadFlags{}.with_state(ThreadState::Dead).get_state() == ThreadState::Dead);
static_assert(ThreadFlags{}.with_uses_amx().with_in_kernel_fpu().raw() == 0x3000000u);
static_assert(!ThreadFlags{}.with_fpu_dirty().get_uses_fpu());
static_assert(ThreadFlags{}.with_nice_bias(20).get_nice_bias() == 20);

// The stack checks mean nothing unless a boot thread knows where its stack really is.
KTEST_ALL_CPUS(Thread, every_cpu_starts_on_a_boot_thread, cpu) {
  Thread* self = sched::current();
  KREQUIRE(self != nullptr);

  KEXPECT_EQ(self->cpu_id(), cpu);
  KEXPECT(self->state() == ThreadState::Running);
  KEXPECT(self->is_idle());
  KEXPECT(self->is_kthread());
  KEXPECT(self->is_pinned());
  KEXPECT(self->flags().get_stack_borrowed());
  KEXPECT(self->name() == "idle");

  KEXPECT_EQ(self->stack_top(), hw::percpu::self()->stack_top);
  KEXPECT_EQ(self->stack_size(), KSTACK_SIZE);
  KEXPECT(self->is_stack_pointer_valid(VirtualAddress{stack_pointer() & ~7ull}));
  KEXPECT(self->verify_stack_canary());
}

KTEST_ALL_CPUS(Thread, boot_threads_have_ids_of_their_own, cpu) {
  static std::atomic<sched::ThreadId> ids[TRACKED_CPUS];

  if (cpu >= TRACKED_CPUS) {
    return;
  }

  ids[cpu].store(sched::current()->tid(), std::memory_order_relaxed);
  sync_cpus();

  for (std::uint32_t other = 0; other < std::min(cpu_count(), TRACKED_CPUS); ++other) {
    if (other != cpu) {
      KEXPECT_NE(ids[other].load(std::memory_order_relaxed), sched::current()->tid());
    }
  }
}

KTEST(Thread, adopting_needs_a_stack_and_a_cpu_without_a_thread) {
  Thread* self = sched::current();

  const auto twice = Thread::adopt_boot_thread(hw::percpu::id(), self->stack_base(), KSTACK_SIZE);
  KREQUIRE(!twice);
  KEXPECT(twice.error() == Error::InvalidState);

  const auto no_stack = Thread::adopt_boot_thread(hw::percpu::id(), VirtualAddress{}, KSTACK_SIZE);
  KREQUIRE(!no_stack);
  KEXPECT(no_stack.error() == Error::InvalidArgument);

  const auto small = Thread::adopt_boot_thread(hw::percpu::id(), self->stack_base(), 4096);
  KREQUIRE(!small);
  KEXPECT(small.error() == Error::InvalidArgument);

  KEXPECT_EQ(sched::current(), self);
}

KTEST(Thread, create_rejects_what_it_cannot_run) {
  const auto no_entry = Thread::create_kernel("bad", nullptr);
  KREQUIRE(!no_entry);
  KEXPECT(no_entry.error() == Error::InvalidArgument);

  const auto too_small =
      Thread::create_kernel("bad", &do_nothing, nullptr, {.stack_size = MIN_KSTACK_SIZE - 16});
  KREQUIRE(!too_small);
  KEXPECT(too_small.error() == Error::InvalidArgument);

  const auto misaligned =
      Thread::create_kernel("bad", &do_nothing, nullptr, {.stack_size = MIN_KSTACK_SIZE + 8});
  KREQUIRE(!misaligned);
  KEXPECT(misaligned.error() == Error::InvalidAlignment);
}

KTEST(Thread, a_new_thread_is_ready_and_has_a_valid_frame) {
  const auto created =
      Thread::create_kernel("worker", &do_nothing, nullptr, {.preferred_cpu = 3, .pinned = true});
  KREQUIRE(created.has_value());
  Thread* thread = *created;

  KEXPECT(thread->state() == ThreadState::Ready);
  KEXPECT(thread->priority_class() == sched::PriorityClass::Normal);
  KEXPECT(thread->is_kthread());
  KEXPECT(!thread->is_idle());
  KEXPECT(thread->is_pinned());
  KEXPECT_EQ(thread->cpu_id(), 3u);
  KEXPECT_NE(thread->tid(), sched::INVALID_THREAD_ID);
  KEXPECT(thread->name() == "worker");
  KEXPECT(thread->stack_base().is_aligned(memory::PAGE_SIZE));
  KEXPECT_EQ(thread->stack_top() - thread->stack_base(), sched::DEFAULT_KSTACK_SIZE);

  const SwitchContext* frame = thread->inspect_switch_context();
  KREQUIRE(frame != nullptr);
  KEXPECT_EQ(frame->r12, reinterpret_cast<std::uint64_t>(thread));
  KEXPECT_NE(frame->rip, 0u);
  KEXPECT_EQ(frame->rbp, 0u);

  // The frame, and the empty return address above it.
  const std::size_t used = sizeof(SwitchContext) + sizeof(std::uint64_t);
  KEXPECT_EQ(thread->stack_usage_bytes(), used);
  KEXPECT_EQ(thread->stack_free_bytes(), sched::DEFAULT_KSTACK_SIZE - used - sizeof(std::uint64_t));

  KEXPECT(Thread::destroy(thread).has_value());
}

KTEST(Thread, stack_sizes_are_rounded_up_to_whole_pages) {
  const auto created =
      Thread::create_kernel("rounded", &do_nothing, nullptr, {.stack_size = MIN_KSTACK_SIZE + 16});
  KREQUIRE(created.has_value());

  KEXPECT_EQ((*created)->stack_size(), MIN_KSTACK_SIZE + memory::PAGE_SIZE);
  KEXPECT_EQ((*created)->stack_top() - (*created)->stack_base(),
             MIN_KSTACK_SIZE + memory::PAGE_SIZE);
  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, long_names_are_cut_short) {
  const auto created =
      Thread::create_kernel("a-name-far-too-long-for-the-thread-control-block", &do_nothing);
  KREQUIRE(created.has_value());

  KEXPECT_EQ((*created)->name().size(), Thread::MAX_NAME_LEN - 1);
  KEXPECT((*created)->name().starts_with("a-name-far"));
  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, stack_pointers_are_checked_against_the_bounds) {
  const auto created = Thread::create_kernel("bounds", &do_nothing);
  KREQUIRE(created.has_value());
  const Thread* thread = *created;

  const VirtualAddress base = thread->stack_base();
  const VirtualAddress top = thread->stack_top();

  KEXPECT(!thread->is_stack_pointer_valid(base));
  KEXPECT(thread->is_stack_pointer_valid(base + 8));
  KEXPECT(thread->is_stack_pointer_valid(top));
  KEXPECT(!thread->is_stack_pointer_valid(top + 8));
  KEXPECT(!thread->is_stack_pointer_valid(base + 12));

  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, a_trampled_canary_is_noticed) {
  const auto created = Thread::create_kernel("canary", &do_nothing);
  KREQUIRE(created.has_value());
  const Thread* thread = *created;

  auto* slot = thread->stack_base().as<volatile std::uint64_t>();
  const std::uint64_t canary = *slot;

  KEXPECT(thread->verify_stack_canary());
  KEXPECT_EQ(canary & 0xFF, 0xA5u);

  *slot = canary ^ 1;
  KEXPECT(!thread->verify_stack_canary());

  *slot = canary;
  KEXPECT(thread->verify_stack_canary());
  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, destroy_refuses_anything_that_is_still_alive) {
  const auto nothing = Thread::destroy(nullptr);
  KREQUIRE(!nothing);
  KEXPECT(nothing.error() == Error::InvalidArgument);

  const auto running = Thread::destroy(sched::current());
  KREQUIRE(!running);
  KEXPECT(running.error() == Error::InvalidState);

  const auto created = Thread::create_kernel("blocked", &do_nothing);
  KREQUIRE(created.has_value());

  (*created)->set_state(ThreadState::Blocked);
  const auto blocked = Thread::destroy(*created);
  KREQUIRE(!blocked);
  KEXPECT(blocked.error() == Error::InvalidState);

  (*created)->set_state(ThreadState::Ready);
  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, destroy_refuses_a_thread_stopped_halfway) {
  PingPong game{.parent = sched::current(), .rounds = 1};
  const auto created = Thread::create_kernel("halfway", &ping_pong_worker, &game);
  KREQUIRE(created.has_value());
  game.worker = *created;

  // Ready, like a thread that never ran, but with half a function left on its stack.
  sched::switch_to(game.parent, game.worker);
  KEXPECT(game.worker->state() == ThreadState::Ready);

  const auto halfway = Thread::destroy(game.worker);
  KREQUIRE(!halfway);
  KEXPECT(halfway.error() == Error::InvalidState);

  run_to_completion(game.worker);
}

namespace {
// Stacks go back to the VMM under memory pressure, so this is how a test gets a cache with room.
void empty_the_stack_cache() noexcept {
  for (std::uint32_t node = 0; node < memory::pmm::g_topology.active_nodes(); ++node) {
    (void)memory::run_shrinkers(node);
  }
}
}  // namespace

// A stack of the usual size stays mapped for the next thread, which gets it without asking the VMM
// for anything. Other threads may come and go meanwhile, so this goes by the counts.
KTEST(Thread, destroy_keeps_a_stack_for_the_next_thread) {
  empty_the_stack_cache();

  const auto created = Thread::create_kernel("stack", &do_nothing);
  KREQUIRE(created.has_value());
  const VirtualAddress stack = (*created)->stack_base();

  const sched::StackCacheStats before = Thread::stack_cache_stats();
  KEXPECT(Thread::destroy(*created).has_value());
  KEXPECT_GT(Thread::stack_cache_stats().cached, before.cached);
  KEXPECT(memory::vmm::kernel_space()->pagemap().translate(stack).has_value());

  const auto next = Thread::create_kernel("next", &do_nothing);
  KREQUIRE(next.has_value());
  KEXPECT_GT(Thread::stack_cache_stats().reused, before.reused);
  KEXPECT(*(*next)->stack_base().as<volatile std::uint64_t>() != 0);
  KEXPECT(Thread::destroy(*next).has_value());
}

// Only the usual size is worth keeping. Anything else is unmapped, so a stack overflow into where
// it was would still fault.
KTEST(Thread, destroy_gives_an_unusual_stack_back) {
  // With room in the cache, or the stack would go back whatever its size.
  empty_the_stack_cache();

  const auto created =
      Thread::create_kernel("big", &do_nothing, nullptr,
                            ThreadCreateOptions{.stack_size = 2 * sched::DEFAULT_KSTACK_SIZE});
  KREQUIRE(created.has_value());
  const VirtualAddress stack = (*created)->stack_base();

  KEXPECT(Thread::destroy(*created).has_value());

  const auto again = memory::vmm::kernel_space()->free(stack);
  KREQUIRE(!again);
  KEXPECT(again.error() == Error::NotMapped);
}

KTEST(Thread, a_new_thread_starts_preemptible_with_interrupts_on) {
  EntryState entry;
  const auto created = Thread::create_kernel("entry", &record_entry_state, &entry);
  KREQUIRE(created.has_value());
  Thread* thread = *created;

  const std::uint8_t preempt_count = hw::percpu::preempt_count();
  run_to_completion(thread);

  KEXPECT_EQ(entry.self, thread);
  KEXPECT_EQ(entry.preempt_count, preempt_count);
  KEXPECT_EQ(hw::percpu::preempt_count(), preempt_count);
  KEXPECT(entry.interrupts_on);
  KEXPECT(entry.can_sleep);
  KEXPECT_EQ(entry.cpu, hw::percpu::id());
}

KTEST(Thread, runs_on_its_own_stack_with_the_alignment_the_abi_wants) {
  std::uint64_t rsp = 0;
  const auto created = Thread::create_kernel("aligned", &record_stack_pointer, &rsp);
  KREQUIRE(created.has_value());

  const std::uint64_t base = (*created)->stack_base().value();
  const std::uint64_t top = (*created)->stack_top().value();
  run_to_completion(*created);

  KEXPECT_GT(rsp, base);
  KEXPECT_LT(rsp, top);
  KEXPECT_EQ(rsp % 16, 8u);
}

KTEST(Thread, switches_there_and_back_as_often_as_it_takes) {
  constexpr std::uint32_t ROUNDS = 1000;

  PingPong game{.parent = sched::current(), .rounds = ROUNDS};
  const auto created = Thread::create_kernel("ping-pong", &ping_pong_worker, &game);
  KREQUIRE(created.has_value());
  game.worker = *created;

  const std::uint64_t parent_switches = game.parent->stats().voluntary_switches;

  for (std::uint32_t round = 0; round < ROUNDS; ++round) {
    sched::switch_to(game.parent, game.worker);

    KEXPECT_EQ(game.turns, round + 1);
    KEXPECT_EQ(sched::current(), game.parent);
    KEXPECT(game.parent->state() == ThreadState::Running);
    KEXPECT(game.worker->state() == ThreadState::Ready);
  }

  KEXPECT_EQ(game.worker->stats().switch_count, ROUNDS);
  KEXPECT_EQ(game.worker->stats().voluntary_switches, ROUNDS);
  KEXPECT_EQ(game.parent->stats().voluntary_switches, parent_switches + ROUNDS);
  KEXPECT(game.worker->verify_stack_canary());

  run_to_completion(game.worker);
}

KTEST(Thread, switching_to_yourself_changes_nothing) {
  Thread* self = sched::current();
  const std::uint64_t switches = self->stats().voluntary_switches;

  sched::switch_to(self, self);

  KEXPECT_EQ(self->stats().voluntary_switches, switches);
  KEXPECT(self->state() == ThreadState::Running);
}

KTEST(Thread, exit_hook_runs_once_the_thread_is_dead) {
  struct Finished {
    Thread* parent;
    ThreadState state;
    std::uint32_t calls;
  } finished{.parent = sched::current(), .state = ThreadState::Created, .calls = 0};

  const auto created = Thread::create_kernel("exit", &do_nothing);
  KREQUIRE(created.has_value());

  (*created)->set_exit_hook(
      [](Thread* self, void* cookie) noexcept {
        auto* result = static_cast<Finished*>(cookie);
        result->state = self->state();
        ++result->calls;
        sched::switch_to(self, result->parent);
      },
      &finished);

  sched::switch_to(finished.parent, *created);

  KEXPECT(finished.state == ThreadState::Dead);
  KEXPECT_EQ(finished.calls, 1u);
  KEXPECT(Thread::destroy(*created).has_value());
}

KTEST(Thread, fs_base_goes_with_the_thread) {
  constexpr std::uint64_t BASE = 0x0000'7FFF'1234'5000;

  std::uint64_t seen = 0;
  const std::uint64_t before = hw::read::fs_base();

  const auto created =
      Thread::create_kernel("fs", &record_fs_base, &seen, {.initial_fs_base = BASE});
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  KEXPECT_EQ(seen, BASE);
  KEXPECT_EQ(hw::read::fs_base(), before);
}

KTEST(Thread, pkru_goes_with_the_thread) {
  if (!hw::percpu::self()->info->has<hw::Feature::PKU>()) {
    KTEST_SKIP("the CPU has no protection keys");
  }

  // Key 1, access disabled. Everything the kernel touches is supervisor memory in key 0 anyway.
  constexpr std::uint32_t RIGHTS = 1u << 2;

  std::uint32_t seen = 0;
  const std::uint32_t before = read_pkru();

  const auto created = Thread::create_kernel("pkru", &record_pkru, &seen);
  KREQUIRE(created.has_value());
  (*created)->arch_state().pkru = RIGHTS;
  run_to_completion(*created);

  KEXPECT_EQ(seen, RIGHTS);
  KEXPECT_EQ(read_pkru(), before);
}

KTEST(Thread, spawn_passes_a_typed_payload) {
  Payload payload{.value = 21};
  const auto spawned = Thread::spawn_kernel("typed", &double_payload, &payload);
  KREQUIRE(spawned.has_value());
  run_to_completion(*spawned);

  KEXPECT_EQ(payload.value, 42u);
}

KTEST(Thread, spawn_rejects_a_null_entry) {
  Payload payload{.value = 0};
  void (*no_entry)(Payload*) noexcept = nullptr;

  const auto spawned = Thread::spawn_kernel("typed", no_entry, &payload);
  KREQUIRE(!spawned);
  KEXPECT(spawned.error() == Error::InvalidArgument);
}

KTEST(Thread, spawn_runs_a_stateless_lambda) {
  g_lambda_runs.store(0, std::memory_order_relaxed);

  const auto spawned = Thread::spawn_kernel(
      "lambda", []() noexcept { g_lambda_runs.fetch_add(1, std::memory_order_relaxed); });
  KREQUIRE(spawned.has_value());
  run_to_completion(*spawned);

  KEXPECT_EQ(g_lambda_runs.load(std::memory_order_relaxed), 1u);
}

KTEST(Thread, threads_go_on_intrusive_lists) {
  const auto first = Thread::create_kernel("first", &do_nothing);
  const auto second = Thread::create_kernel("second", &do_nothing);
  KREQUIRE(first.has_value());
  KREQUIRE(second.has_value());

  using Link = utils::SListNode<Thread, sched::RunQueueTag>;

  utils::SList<Thread, sched::RunQueueTag> queue;
  queue.push_front(**first);
  queue.push_front(**second);

  KEXPECT(static_cast<Link*>(*first)->is_linked());
  KEXPECT(queue.pop_front() == *second);
  KEXPECT(queue.pop_front() == *first);
  KEXPECT(queue.empty());
  KEXPECT(!static_cast<Link*>(*first)->is_linked());

  KEXPECT(Thread::destroy(*first).has_value());
  KEXPECT(Thread::destroy(*second).has_value());
}

KTEST(Thread, survives_many_lives) {
  for (std::uint32_t i = 0; i < 256; ++i) {
    const auto created =
        Thread::create_kernel("short-lived", &do_nothing, nullptr, {.allocate_fpu = i % 2 == 0});
    KREQUIRE(created.has_value());
    run_to_completion(*created);
  }
}

KTEST_ALL_CPUS(Thread, every_cpu_switches_at_the_same_time, cpu) {
  sync_cpus();
  KEXPECT_EQ(play_fpu_workers(cpu * 10 + 1, 100), 0u);
  KEXPECT_EQ(sched::current()->cpu_id(), cpu);
}

// A thread stopped on one CPU and resumed on another. Everything per-CPU has to follow it.
KTEST_ALL_CPUS(Thread, moves_between_cpus_with_its_fpu_state, cpu) {
  if (cpu_count() > TRACKED_CPUS) {
    KTEST_SKIP("more CPUs than the test tracks");
  }

  if (cpu == 0) {
    g_visitor_turn.store(0, std::memory_order_relaxed);
    g_visits.store(0, std::memory_order_relaxed);
    g_visit_mismatches.store(0, std::memory_order_relaxed);
    g_wrong_cpu.store(0, std::memory_order_relaxed);
    g_journey_over.store(false, std::memory_order_relaxed);

    const auto created = Thread::create_kernel("traveller", &traveller);
    KREQUIRE(created.has_value());
    g_traveller = *created;
  }

  sync_cpus();

  while (g_visitor_turn.load(std::memory_order_acquire) != cpu) {
    hw::pause();
  }

  g_homes[cpu] = sched::current();
  sched::switch_to(g_homes[cpu], g_traveller);
  g_visitor_turn.store(cpu + 1, std::memory_order_release);

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(g_visits.load(std::memory_order_relaxed), cpu_count());
    KEXPECT_EQ(g_visit_mismatches.load(std::memory_order_relaxed), 0u);
    KEXPECT_EQ(g_wrong_cpu.load(std::memory_order_relaxed), 0u);

    g_journey_over.store(true, std::memory_order_release);
    run_to_completion(g_traveller);
  }
}

KTEST(ThreadFpu, context_only_comes_on_request) {
  const auto created = Thread::create_kernel("no-fpu", &do_nothing);
  KREQUIRE(created.has_value());
  Thread* thread = *created;

  KEXPECT(!thread->uses_fpu());
  KEXPECT_EQ(thread->arch_state().fpu_storage, nullptr);
  KEXPECT_EQ(thread->arch_state().fpu_buffer_size, 0u);

  KEXPECT(thread->enable_fpu_context().has_value());
  void* area = thread->arch_state().fpu_storage;

  KEXPECT(thread->uses_fpu());
  KEXPECT(!thread->uses_amx());
  KREQUIRE(area != nullptr);
  KEXPECT_EQ(reinterpret_cast<std::uintptr_t>(area) % fpu::AREA_ALIGNMENT, 0u);
  KEXPECT_EQ(thread->arch_state().fpu_buffer_size, fpu::area_size());
  KEXPECT_EQ(thread->arch_state().xsave_mask, fpu::state_mask());

  KEXPECT(thread->enable_fpu_context().has_value());
  KEXPECT_EQ(thread->arch_state().fpu_storage, area);

  KEXPECT(Thread::destroy(thread).has_value());
}

// Without XSTATE_BV and XCOMP_BV just right, the XRSTOR of a first switch is a #GP.
KTEST(ThreadFpu, a_new_context_starts_clean_whatever_ran_before) {
  struct Report {
    RegisterFile registers{};
  } report;

  utils::NullLock lock;
  {
    utils::FpuGuard guard{lock};
    load_registers(make_pattern(5));
  }

  const auto created = Thread::spawn_kernel("clean",
                                            +[](Report* result) noexcept {
                                              utils::NullLock inner;
                                              utils::FpuGuard guard{inner};
                                              store_registers(result->registers);
                                            },
                                            &report, {.allocate_fpu = true});
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  KEXPECT(report.registers == RegisterFile{});
}

KTEST(ThreadFpu, a_thread_without_a_context_leaves_the_registers_alone) {
  const RegisterFile mine = make_pattern(6);
  utils::NullLock lock;
  {
    utils::FpuGuard guard{lock};
    load_registers(mine);
  }

  const auto created = Thread::create_kernel("no-fpu", &do_nothing);
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  RegisterFile found{};
  {
    utils::FpuGuard guard{lock};
    store_registers(found);
  }

  KEXPECT(found == mine);
}

KTEST(ThreadFpu, two_threads_keep_their_registers_apart) {
  KEXPECT_EQ(play_fpu_workers(100, 200), 0u);
}

// The thread's save area has to be as good as the registers after going through a dirty switch.
KTEST(ThreadFpu, a_switch_clears_what_it_saved) {
  utils::NullLock lock;
  {
    utils::FpuGuard guard{lock};
    load_registers(make_pattern(11));
  }

  Thread* self = sched::current();
  KEXPECT(self->flags().get_fpu_dirty());

  const auto created = Thread::create_kernel("clean", &do_nothing);
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  KEXPECT(!self->flags().get_fpu_dirty());
}

KTEST(ThreadFpu, amx_needs_a_cpu_with_amx) {
  if (fpu::amx_available()) {
    KTEST_SKIP("the CPU has AMX");
  }

  const auto created = Thread::create_kernel("no-amx", &do_nothing);
  KREQUIRE(created.has_value());

  const auto amx = (*created)->request_amx_context();
  KREQUIRE(!amx);
  KEXPECT(amx.error() == Error::NotImplemented);
  KEXPECT(!(*created)->uses_amx());
  KEXPECT(Thread::destroy(*created).has_value());

  const auto refused =
      Thread::create_kernel("no-amx", &do_nothing, nullptr, {.allocate_amx = true});
  KREQUIRE(!refused);
  KEXPECT(refused.error() == Error::NotImplemented);
}

KTEST(ThreadFpu, amx_extends_the_context_and_keeps_what_was_in_it) {
  if (!fpu::amx_available()) {
    KTEST_SKIP("the CPU has no AMX");
  }

  const auto created = Thread::create_kernel("amx", &do_nothing, nullptr, {.allocate_fpu = true});
  KREQUIRE(created.has_value());
  Thread* thread = *created;

  // Something an area just built from scratch would not have.
  auto* legacy = static_cast<fpu::LegacyArea*>(thread->arch_state().fpu_storage);
  legacy->mxcsr = fpu::DEFAULT_MXCSR | (1u << 13);

  KEXPECT(thread->request_amx_context().has_value());
  KEXPECT(thread->uses_amx());
  KEXPECT_EQ(thread->arch_state().fpu_buffer_size, fpu::area_size(true));
  KEXPECT_EQ(thread->arch_state().xsave_mask, fpu::state_mask(true));
  KEXPECT_EQ(thread->arch_state().xfd.raw(), 0u);

  legacy = static_cast<fpu::LegacyArea*>(thread->arch_state().fpu_storage);
  KEXPECT_EQ(legacy->mxcsr, fpu::DEFAULT_MXCSR | (1u << 13));

  KEXPECT(Thread::destroy(thread).has_value());
}

// LDTILECFG works with tile data disabled, so a thread can have a tile configuration without ever
// asking for AMX. Goes by the save areas, as they are what a switch has to get right.
KTEST(ThreadFpu, tile_configuration_stays_with_the_thread_that_loaded_it) {
  if (!fpu::amx_available()) {
    KTEST_SKIP("the CPU has no AMX");
  }

  Thread* self = sched::current();
  TileWorker loader{.parent = self, .loads_config = true};
  TileWorker bystander{.parent = self};

  const auto first =
      Thread::create_kernel("tile-loader", &tile_worker, &loader, {.allocate_fpu = true});
  const auto second =
      Thread::create_kernel("tile-bystander", &tile_worker, &bystander, {.allocate_fpu = true});
  KREQUIRE(first.has_value());
  KREQUIRE(second.has_value());
  loader.self = *first;
  bystander.self = *second;

  // Saved on the way out, and gone for the next thread to run...
  sched::switch_to(self, loader.self);
  KEXPECT(has_saved_tile_config(loader.self));

  sched::switch_to(self, bystander.self);
  KEXPECT(!has_saved_tile_config(bystander.self));

  // ...so only a restore can have brought it back for this save to find...
  sched::switch_to(self, loader.self);
  KEXPECT(has_saved_tile_config(loader.self));

  // ...and it goes again for the bystander, however recently it was in the registers.
  sched::switch_to(self, bystander.self);
  KEXPECT(!has_saved_tile_config(bystander.self));

  run_to_completion(loader.self);
  run_to_completion(bystander.self);
}

KTEST(ThreadFpu, xfd_follows_the_amx_thread) {
  if (!fpu::amx_available()) {
    KTEST_SKIP("the CPU has no AMX");
  }

  std::uint64_t seen = ~0ul;
  const auto created = Thread::spawn_kernel(
      "amx", +[](std::uint64_t* xfd) noexcept { *xfd = hw::read::msr(fpu::MSR_IA32_XFD); }, &seen,
      {.allocate_amx = true});
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  KEXPECT_EQ(seen, 0u);
  KEXPECT_EQ(hw::read::msr(fpu::MSR_IA32_XFD), fpu::XFD_NO_AMX.raw());
}

KTEST(FpuGuard, holds_the_lock_and_keeps_the_cpu_for_as_long_as_it_lives) {
  utils::TicketSpinlock lock;
  const std::uint8_t preempt_count = hw::percpu::preempt_count();

  {
    utils::FpuGuard guard{lock};

    KEXPECT(guard.context_ready());
    KEXPECT(!lock.try_lock());
    KEXPECT_GT(hw::percpu::preempt_count(), preempt_count);
    KEXPECT(sched::current()->in_kernel_fpu());
  }

  KEXPECT_EQ(hw::percpu::preempt_count(), preempt_count);
  KEXPECT(!sched::current()->in_kernel_fpu());
  KEXPECT(lock.try_lock());
  lock.unlock();
}

KTEST(FpuGuard, gives_a_thread_its_context_on_first_use) {
  struct Report {
    bool before{true};
    bool ready{false};
    bool after{false};
  } report;

  const auto created = Thread::spawn_kernel(
      "first-use",
      +[](Report* result) noexcept {
        Thread* self = sched::current();
        result->before = self->uses_fpu();

        utils::NullLock lock;
        utils::FpuGuard guard{lock};
        result->ready = guard.context_ready();
        result->after = self->uses_fpu();
      },
      &report);
  KREQUIRE(created.has_value());
  run_to_completion(*created);

  KEXPECT(!report.before);
  KEXPECT(report.ready);
  KEXPECT(report.after);
}

KTEST(FpuGuard, nested_sections_are_one_section) {
  utils::NullLock lock;
  Thread* self = sched::current();
  const RegisterFile outer = make_pattern(21);
  RegisterFile found{};
  const std::uint8_t preempt_count = hw::percpu::preempt_count();

  {
    utils::FpuGuard guard{lock};
    load_registers(outer);

    {
      utils::FpuGuard inner{lock};
      KEXPECT(self->in_kernel_fpu());
    }

    // The inner one must not have saved or restored anything, or left the section.
    KEXPECT(self->in_kernel_fpu());
    store_registers(found);
  }

  KEXPECT(found == outer);
  KEXPECT(!self->in_kernel_fpu());
  KEXPECT(self->flags().get_fpu_dirty());
  KEXPECT_EQ(hw::percpu::preempt_count(), preempt_count);
}

KTEST(FpuGuard, what_a_section_leaves_is_there_for_the_next) {
  utils::NullLock lock;
  Thread* self = sched::current();
  const std::uint64_t seed = 31;

  // More than once: every section has to behave the same, whatever the one before it did.
  for (std::uint64_t round = 0; round < 4; ++round) {
    RegisterFile found{};
    {
      utils::FpuGuard guard{lock};
      store_registers(found);
      load_registers(make_pattern(seed + round));
    }

    KEXPECT(self->flags().get_fpu_dirty());
    if (round > 0) {
      KEXPECT(found == make_pattern(seed + round - 1));
    }
  }
}

KTEST(FpuGuard, says_so_when_amx_is_not_there) {
  if (fpu::amx_available()) {
    KTEST_SKIP("the CPU has AMX");
  }

  utils::NullLock lock;
  utils::FpuGuard guard{lock, true};
  KEXPECT(!guard.context_ready());
}
}  // namespace kernel::testing
