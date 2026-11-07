/// @file timer.hpp
/// @brief One-shot and periodic timers, per CPU, with deadlines in nanoseconds.
///
/// @code
/// hw::timer::Timer timeout{&on_timeout, &request};
/// timeout.start_after(5_ms);
/// ...
/// timeout.cancel_sync();  // nothing runs after this, so `request` can go away
/// @endcode
///
/// Every CPU has a TimerQueue in its hw::PerCpu, and one hardware timer it programs for whatever is
/// due first. A Timer lives wherever its owner wants it, in a struct or on a stack, and holds all
/// it needs, so starting one cannot fail and works from any context, interrupts included. Callbacks
/// run in interrupt context on the CPU the timer was started on.
///
/// ### Hardware
///
/// All that a timer needs from a CPU comes down to two local APIC modes, and which one a machine
/// has is not a matter of who made it, so the code only ever asks CPUID:
///
///  - TSC-deadline: an absolute TSC value in an MSR, good to one TSC tick, which is under a
///    nanosecond. Most Intel parts of the last decade have it, and AMD ones not always: a Ryzen 5
///    8645HS, which is Zen 4, does not report it.
///  - One-shot: a 32-bit countdown from the APIC's own clock, whose rate is measured at boot. It
///    counts far slower than the TSC, and its range is limited, so a far-away deadline goes in
///    several legs, which nobody sees.
///
/// Either way a timer that would stop in a deep C-state could be a problem, but the idle loop never
/// goes deeper than C1, where both keep running.
///
/// ### Nanoseconds
///
/// Deadlines, and everything computed from them, are in nanoseconds of drivers::SystemClock, and
/// nothing rounds them to a tick. Two things stand between that and a callback running at the
/// right moment:
///
///  - Converting a deadline into hardware units. That is done from the time left rather than from
///    the time since boot: the conversion factors are rounded, and an error proportional to uptime
///    would come to some 20 us after a day at 3 GHz, and keep growing. Conversions round up. A
///    timer is late, never early.
///  - Interrupts take time to arrive: the better part of a microsecond in a good case, and much
///    longer if the CPU is in a sleep state or a hypervisor is in the way.
///
/// For the second one, a TimerPrecision::Exact timer goes off early by the time the CPU has been
/// taking to get here, which it keeps measuring, and spins on the TSC for the rest. That costs
/// that much CPU time with interrupts off, on every expiry, so it is opt-in. The result is limited
/// by how fast the TSC can be read, which makes for a typical error of a few tens of nanoseconds on
/// either kind of hardware timer, and by whatever else has interrupts disabled: nothing can start a
/// callback while something holds up interrupts for longer than the measured time.
///
/// ### Not reprogramming the hardware
///
/// In a VM every write to the timer is an exit to the hypervisor, and starting and cancelling is
/// what timers mostly do. So the hardware is only touched when a timer becomes the first to expire.
/// Cancelling the first one leaves the hardware armed; it goes off for nothing, and finds out what
/// is next then. Timeouts mostly get cancelled, so that interrupt mostly replaces many writes.
///
/// A CPU about to go idle is the exception. Nothing is running that an early interrupt would only
/// briefly disturb, and it would wake the CPU for nothing, so TimerQueue::prepare_for_idle() is
/// worth one write. With no timers waiting, nothing goes off on an idle CPU at all.
#pragma once

#include <atomic>
#include <chrono.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

#include "core/errors.hpp"
#include "utils/avl_tree.hpp"
#include "utils/locks/locks.hpp"

namespace kernel::hw::timer {
class TimerQueue;

/// @brief What a callback wants to happen to its timer.
enum class TimerAction : std::uint8_t {
  Done,     ///< Nothing more: it stays stopped unless somebody starts it.
  Restart,  ///< Go again at the deadline it has now, which Timer::forward() is for.
};

/// @brief How much a timer is worth to the CPU.
enum class TimerPrecision : std::uint8_t {
  Normal,  ///< Runs as soon as the interrupt arrives, which is after the deadline.
  Exact,   ///< Runs at the deadline, spinning for the last stretch. See the file comment.
};

/// @brief A timer. See the file comment.
///
/// One can be started, restarted and cancelled from any CPU at any moment, including from its own
/// callback, and calls may race with each other and with the expiry. The one rule is about going
/// away: the owner has to see cancel_sync() through first.
class Timer {
 public:
  /// @brief Runs in interrupt context with interrupts off. May start and cancel timers, and wake
  ///        threads. Must not sleep, and should be short: the rest of the queue is waiting.
  using Callback = TimerAction (*)(Timer& timer, void* context) noexcept;

 private:
  friend class TimerQueue;

  // Its links in the queue's tree. A member rather than a base, and the first one: that keeps a
  // Timer, and everything with one inside, a standard-layout type, which the per-CPU area has to
  // be for its offsetof() accessors. It also makes the links and the timer the same address.
  utils::AvlNode m_node{};

  std::uint64_t m_fire_ns{0};      // Order in the queue: when the hardware has to go off.
  std::uint64_t m_earliest_ns{0};  // Does not run before this, whatever else is going on.
  std::uint64_t m_deadline_ns{0};  // What was asked for.
  std::uint64_t m_slack_ns{0};

  Callback m_callback{nullptr};
  void* m_context{nullptr};
  TimerPrecision m_precision{TimerPrecision::Normal};

  std::atomic<TimerQueue*> m_queue{nullptr};       // Waiting on it. Null when not.
  std::atomic<TimerQueue*> m_running_on{nullptr};  // Whose CPU has the callback going.

 public:
  constexpr Timer() noexcept = default;

  constexpr explicit Timer(const Callback callback, void* context = nullptr,
                           const TimerPrecision precision = TimerPrecision::Normal) noexcept
      : m_callback(callback), m_context(context), m_precision(precision) {}

  /// @brief Sets what a default-constructed timer does.
  /// @pre The timer has not been started.
  constexpr void set_callback(const Callback callback, void* context = nullptr,
                              const TimerPrecision precision = TimerPrecision::Normal) noexcept {
    m_callback = callback;
    m_context = context;
    m_precision = precision;
  }

  /// @brief Starts the timer on the calling CPU, or moves it there if it was running elsewhere.
  ///
  /// A deadline in the past fires right away.
  /// @param deadline The callback runs at this moment or after, never before.
  /// @param slack    How long after @p deadline it may run, if that saves an interrupt. Timers
  ///                 close to each other then share one. Ignored by TimerPrecision::Exact.
  /// @return Whether it was waiting already, so that this replaced a deadline.
  bool start_at(klib::TimePoint deadline, klib::Duration slack = {}) noexcept;

  /// @brief start_at() @p delay from now.
  bool start_after(klib::Duration delay, klib::Duration slack = {}) noexcept;

  /// @brief start_at(), on CPU @p cpu.
  /// @return Error::InvalidState if that CPU is not online.
  auto start_on(std::uint32_t cpu, klib::TimePoint deadline, klib::Duration slack = {}) noexcept
      -> std::expected<bool, Error>;

  /// @brief Stops the timer, if it has not started running.
  /// @return Whether that stopped it. If not, the callback may still be running right now.
  bool cancel() noexcept;

  /// @brief Stops the timer and waits for a callback that is running on another CPU to finish, so
  ///        that nothing of it is left once this returns.
  ///
  /// From its own callback this is the same as cancel(), since waiting for itself would not end.
  /// @pre Interrupts are on, or the callback must not be waiting for this CPU to do something.
  void cancel_sync() noexcept;

  /// @brief For a callback that wants to go on at regular intervals: moves the deadline on by as
  ///        many @p period as it takes to be in the future.
  ///
  /// The new deadline comes from the old one and not from the time now, so a periodic timer does
  /// not drift, however late each callback happens to run. Return TimerAction::Restart afterwards.
  /// @return How many periods that took: 1 if all is well, more if some were missed.
  std::uint64_t forward(klib::Duration period) noexcept;

  /// @brief forward(), as if the time were @p now.
  std::uint64_t forward(klib::Duration period, klib::TimePoint now) noexcept;

  /// @brief Whether the timer is waiting to go off. A running callback does not count.
  [[nodiscard]] bool is_pending() const noexcept {
    return m_queue.load(std::memory_order_acquire) != nullptr;
  }

  /// @brief The deadline it was last given.
  [[nodiscard]] klib::TimePoint deadline() const noexcept { return klib::TimePoint{m_deadline_ns}; }
};

/// @brief How long interrupts take on a CPU, as far as it can tell.
///
/// Works the way TCP estimates round-trip times, for the same reason: it wants a number that is
/// rarely too low, without being far too high, from samples that come with noise. It keeps a
/// moving average of the latency and of how much that varies, and adds a few of the latter on top.
class LatencyEstimator {
  std::uint64_t m_mean_ns{DEFAULT_MEAN_NS};
  std::uint64_t m_deviation_ns{DEFAULT_DEVIATION_NS};
  std::uint64_t m_samples{0};

 public:
  /// @brief The guess before the first measurement.
  static constexpr std::uint64_t DEFAULT_MEAN_NS = 1000;
  static constexpr std::uint64_t DEFAULT_DEVIATION_NS = 250;

  /// @brief Latencies above this are something else keeping interrupts off, and tell nothing about
  ///        the CPU. It is also the longest an Exact timer spins.
  static constexpr std::uint64_t MAX_LATENCY_NS = 50'000;

  /// @brief Takes a measurement into account.
  constexpr void observe(const std::uint64_t latency_ns) noexcept {
    if (latency_ns > MAX_LATENCY_NS) {
      return;
    }

    const std::int64_t error =
        static_cast<std::int64_t>(latency_ns) - static_cast<std::int64_t>(m_mean_ns);
    const std::int64_t size = error < 0 ? -error : error;

    m_mean_ns = static_cast<std::uint64_t>(static_cast<std::int64_t>(m_mean_ns) + error / 8);
    m_deviation_ns =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(m_deviation_ns) +
                                   (size - static_cast<std::int64_t>(m_deviation_ns)) / 4);
    ++m_samples;
  }

  /// @brief How many measurements it took into account. Once it has settled, the estimate itself
  ///        can stay put through thousands of them, as the averages move in whole nanoseconds.
  [[nodiscard]] constexpr std::uint64_t samples() const noexcept { return m_samples; }

  /// @brief How early to go off for the interrupt to almost always be there in time.
  [[nodiscard]] constexpr std::uint64_t advance_ns() const noexcept {
    const std::uint64_t advance = m_mean_ns + 4 * m_deviation_ns;
    return advance < MAX_LATENCY_NS ? advance : MAX_LATENCY_NS;
  }
};

/// @brief What a CPU's timers have been up to.
struct TimerStats {
  std::uint64_t expired{0};        ///< Callbacks run.
  std::uint64_t hardware_arms{0};  ///< Writes to the hardware timer.
  std::uint64_t idle_wakeups{0};   ///< Interrupts that found nothing to do.
  std::uint64_t max_late_ns{0};    ///< The latest a callback has started after its deadline.
};

/// @brief The timers of one CPU. See the file comment.
///
/// Usually reached through Timer. A queue that was not attached to a CPU has no hardware under it
/// and only moves when expire() is called, with any time at all. The tests like that.
class TimerQueue {
  struct Order {
    [[nodiscard]] static bool less(const Timer& a, const Timer& b) noexcept {
      return a.m_fire_ns < b.m_fire_ns;
    }

    static void update(Timer&) noexcept {}

    [[nodiscard]] static const utils::AvlNode& node(const Timer& timer) noexcept {
      return timer.m_node;
    }

    // The links are the first member of a standard-layout class, so the two pointers are
    // interconvertible.
    [[nodiscard]] static const Timer* item(const utils::AvlNode* node) noexcept {
      return reinterpret_cast<const Timer*>(node);
    }
  };

  static_assert(std::is_standard_layout_v<Timer> && offsetof(Timer, m_node) == 0);

  using Tree = utils::AvlTree<Timer, Order>;

  static constexpr std::uint64_t NOT_ARMED = ~std::uint64_t{0};

  // A pass ends here and the rest goes through another interrupt, so that a timer whose callback
  // keeps restarting it in the past cannot keep the CPU from everything else.
  static constexpr std::uint32_t MAX_EXPIRIES_PER_PASS = 64;

  utils::QSpinlock m_lock{};
  Tree m_tree{};
  Timer* m_first{nullptr};
  bool m_expiring{false};  // In a pass, which programs the hardware when it is done.

  bool m_has_hardware{false};
  std::uint32_t m_cpu{0};
  std::uint64_t m_armed_ns{NOT_ARMED};  // When the hardware goes off.
  std::uint64_t m_hardware_limit_ns{~std::uint64_t{0}};

  LatencyEstimator m_latency{};
  TimerStats m_stats{};

  void link_locked(Timer& timer) noexcept;
  void unlink_locked(Timer& timer) noexcept;
  void reprogram_locked() noexcept;
  void run(Timer& timer, std::uint64_t& now_ns) noexcept;

  friend class Timer;

 public:
  constexpr TimerQueue() noexcept = default;

  TimerQueue(const TimerQueue&) = delete;
  TimerQueue& operator=(const TimerQueue&) = delete;

  /// @brief Connects the queue to the calling CPU's hardware.
  /// @param cpu      The calling CPU.
  /// @param limit_ns The longest the hardware can wait in one go.
  void attach(std::uint32_t cpu, std::uint64_t limit_ns) noexcept;

  /// @brief Makes the hardware go off sooner than it has to. Long waits take more legs then, which
  ///        is how a test gets to see that without waiting for a minute.
  void limit_hardware_wait(std::uint64_t limit_ns) noexcept;

  void lock() noexcept { m_lock.lock(); }
  void unlock() noexcept { m_lock.unlock(); }

  /// @brief Puts @p timer on this queue. Timer::start_at() on a specific queue.
  /// @return Whether it was waiting already, on this queue or another.
  bool add(Timer& timer, klib::TimePoint deadline, klib::Duration slack = {}) noexcept;

  /// @brief Runs everything that is due at @p now_ns.
  /// @return How many callbacks ran.
  std::uint32_t expire(std::uint64_t now_ns) noexcept;

  /// @brief What the timer interrupt does. @pre On the queue's CPU, with interrupts off.
  void on_interrupt() noexcept;

  /// @brief Programs the hardware for what is first now. @pre On the queue's CPU.
  void reprogram() noexcept;

  /// @brief Moves the hardware to what is really next, or stops it if nothing is, so that an
  ///        idle CPU gets no interrupt that has nothing to do. @pre On the queue's CPU.
  void prepare_for_idle() noexcept;

  /// @brief When the first timer wants to go off, or null if there is none.
  [[nodiscard]] const Timer* first() const noexcept { return m_first; }

  /// @brief How early an Exact timer goes off right now.
  [[nodiscard]] std::uint64_t advance_ns() const noexcept { return m_latency.advance_ns(); }

  /// @brief How many interrupt latencies went into advance_ns() so far.
  [[nodiscard]] std::uint64_t latency_samples() const noexcept { return m_latency.samples(); }

  [[nodiscard]] TimerStats stats() const noexcept { return m_stats; }

  /// @brief Checks the queue's invariants. O(n), for tests.
  [[nodiscard]] bool is_consistent() const noexcept;
};

/// @brief Installs the timer interrupt. Once, on the BSP.
void initialize() noexcept;

/// @brief Sets up the calling CPU's hardware timer and connects its queue.
/// @pre Its local APIC is up, and the clock is calibrated.
void initialize_cpu() noexcept;
}  // namespace kernel::hw::timer
