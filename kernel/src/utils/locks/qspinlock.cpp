#include "hal/smp/percpu.hpp"
#include "hal/smp/smp.hpp"
#include "utils/lock.hpp"

namespace kernel::utils {
namespace {
// Layout of the lock word:
//
//   bits  0- 7  locked byte, owned by the lock holder
//   bit      8  pending, set by the one waiter that spins without queueing
//   bits  9-10  node index of the CPU at the tail of the queue
//   bits 11-31  that CPU's ID plus one, so that 0 means "no queue"
constexpr std::uint32_t LOCKED_VAL = 1u << 0;
constexpr std::uint32_t LOCKED_MASK = 0xFF;
constexpr std::uint32_t PENDING_VAL = 1u << 8;
constexpr std::uint32_t PENDING_MASK = PENDING_VAL;
constexpr std::uint32_t LOCKED_PENDING_MASK = LOCKED_MASK | PENDING_MASK;
constexpr std::uint32_t TAIL_MASK = ~LOCKED_PENDING_MASK;
constexpr std::uint32_t TAIL_IDX_SHIFT = 9;
constexpr std::uint32_t TAIL_CPU_SHIFT = 11;

static_assert(hw::smp::MAX_CPUS <= QSpinlock::MAX_CPUS, "the tail cannot name every CPU");
static_assert(QSpinlock::MAX_CPUS == TAIL_MASK >> TAIL_CPU_SHIFT);

constexpr std::uint32_t PENDING_LOOPS = 512;
static_assert(QSpinlock::MAX_NESTING <= 4, "the tail has two bits for the node index");

[[nodiscard]] constexpr std::uint32_t encode_tail(const std::uint32_t cpu,
                                                  const std::uint32_t idx) noexcept {
  return ((cpu + 1) << TAIL_CPU_SHIFT) | (idx << TAIL_IDX_SHIFT);
}

[[nodiscard]] McsNode* decode_tail(const std::uint32_t tail) noexcept {
  const std::uint32_t cpu = (tail >> TAIL_CPU_SHIFT) - 1;
  const std::uint32_t idx = (tail >> TAIL_IDX_SHIFT) & 0x3;
  return &hw::percpu::cpu_area(cpu)->qnodes[idx];
}

// Waits until `done(value)` holds for the word and returns that value.
template <typename Pred>
[[nodiscard]] std::uint32_t wait_until(std::atomic<std::uint32_t>& word, Pred&& done,
                                       const std::memory_order order) noexcept {
  std::uint32_t val = word.load(order);
  while (!done(val)) {
    detail::wait_monitor(&word, val);
    val = word.load(order);
  }

  return val;
}
}  // namespace

bool QSpinlock::try_lock() noexcept {
  std::uint32_t val = m_val.load(std::memory_order_relaxed);
  return val == 0 && m_val.compare_exchange_strong(val, LOCKED_VAL, std::memory_order_acquire,
                                                   std::memory_order_relaxed);
}

void QSpinlock::lock() noexcept {
  std::uint32_t val = 0;
  if (m_val.compare_exchange_strong(val, LOCKED_VAL, std::memory_order_acquire,
                                    std::memory_order_relaxed)) [[likely]] {
    return;
  }

  slow_path_lock(val);
}

void QSpinlock::unlock() noexcept {
  // Nobody but the holder touches the locked byte, so this cannot disturb the other fields.
  m_val.fetch_sub(LOCKED_VAL, std::memory_order_release);
}

void QSpinlock::slow_path_lock(std::uint32_t val) noexcept {
  // Pending set but no owner means a waiter is in the middle of taking the lock. Give it a
  // moment instead of queueing behind a lock that is about to change hands anyway.
  if (val == PENDING_VAL) {
    for (std::uint32_t loops = PENDING_LOOPS; val == PENDING_VAL && loops > 0; --loops) {
      hw::pause();
      val = m_val.load(std::memory_order_relaxed);
    }
  }

  // With only an owner and nobody waiting, try to become the pending waiter. It spins on the
  // lock word directly, which saves the queue node round trip for the common two-CPU case.
  if ((val & ~LOCKED_MASK) == 0) {
    val = m_val.fetch_or(PENDING_VAL, std::memory_order_acquire);

    if ((val & ~LOCKED_MASK) == 0) {
      if (val & LOCKED_MASK) {
        (void)wait_until(
            m_val, [](const std::uint32_t v) { return (v & LOCKED_MASK) == 0; },
            std::memory_order_acquire);
      }

      // pending=1, locked=0 -> pending=0, locked=1 in one step. Nothing else can set the
      // locked byte while pending is set, and the tail may change concurrently, hence an add.
      m_val.fetch_add(LOCKED_VAL - PENDING_VAL, std::memory_order_relaxed);
      return;
    }

    // Lost the race to someone else. Take our pending bit back, if it was ours, and queue.
    if ((val & PENDING_MASK) == 0) {
      m_val.fetch_and(~PENDING_VAL, std::memory_order_relaxed);
    }
  }

  queue_and_lock();
}

void QSpinlock::queue_and_lock() noexcept {
  hw::PerCpu* self = hw::percpu::self();
  McsNode* nodes = self->qnodes.data();

  // An interrupt arriving now may take node `idx` for a lock of its own. That is fine: it is
  // done with the node before we get to run again, which is also why a plain increment is safe.
  const std::uint32_t idx = nodes[0].count++;
  std::atomic_signal_fence(std::memory_order_seq_cst);

  const auto release_node = [&] {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    nodes[0].count--;
  };

  if (idx >= MAX_NESTING) [[unlikely]] {
    // Nested deeper than we have nodes for. Rare enough that plain spinning will do.
    while (!try_lock()) {
      hw::pause();
    }

    release_node();
    return;
  }

  McsNode* node = &nodes[idx];
  const std::uint32_t tail = encode_tail(hw::percpu::id(), idx);

  node->locked.store(0, std::memory_order_relaxed);
  node->next.store(nullptr, std::memory_order_relaxed);

  // The lock may have become free while we were getting here.
  if (try_lock()) {
    release_node();
    return;
  }

  // Become the new tail. Release publishes the node initialisation to whoever finds the node
  // through the lock word; acquire lets us use the previous tail's node.
  std::uint32_t old = m_val.load(std::memory_order_relaxed);
  while (!m_val.compare_exchange_weak(old, (old & LOCKED_PENDING_MASK) | tail,
                                      std::memory_order_acq_rel, std::memory_order_relaxed)) {
  }

  McsNode* next = nullptr;
  if (old & TAIL_MASK) {
    decode_tail(old)->next.store(node, std::memory_order_release);

    // Wait to be handed the front of the queue, spinning on our own cache line.
    (void)wait_until(
        node->locked, [](const std::uint32_t v) { return v != 0; }, std::memory_order_acquire);
    next = node->next.load(std::memory_order_acquire);
  }

  // At the front. Wait for the owner and any pending waiter; nobody can get ahead of us now,
  // since both the fast path and the pending path back off when the tail is set.
  std::uint32_t val = wait_until(
      m_val, [](const std::uint32_t v) { return (v & LOCKED_PENDING_MASK) == 0; },
      std::memory_order_acquire);

  // Still the tail: take the lock and empty the queue in one go.
  if ((val & TAIL_MASK) == tail &&
      m_val.compare_exchange_strong(val, LOCKED_VAL, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
    release_node();
    return;
  }

  // Others queued behind us. The locked byte is ours to set, then the front of the queue goes
  // to our successor, who may not have finished linking itself in yet.
  m_val.fetch_or(LOCKED_VAL, std::memory_order_relaxed);

  while (!next) {
    hw::pause();
    next = node->next.load(std::memory_order_acquire);
  }

  next->locked.store(1, std::memory_order_release);
  release_node();
}
}  // namespace kernel::utils
