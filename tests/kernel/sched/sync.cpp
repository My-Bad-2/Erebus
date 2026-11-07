// What is built on top of WaitQueue. The queue itself has its own tests next door, so these are
// about the promises each of these makes on top of it.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hal/timer/timer.hpp"
#include "ktest.hpp"
#include "sched/completion.hpp"
#include "sched/condvar.hpp"
#include "sched/mutex.hpp"
#include "sched/semaphore.hpp"
#include "support.hpp"
#include "utils/lock.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using sched::Completion;
using sched::CondVar;
using sched::Latch;
using sched::Mutex;
using sched::Semaphore;
using sched::Thread;
using support::A_MOMENT;
using support::start;
using support::wait_for;

constexpr std::uint32_t THREADS = 6;

struct Taker {
  Semaphore* semaphore{nullptr};
  std::atomic<std::uint32_t>* done{nullptr};
};

void take_one(Taker* taker) noexcept {
  taker->semaphore->acquire();
  taker->done->fetch_add(1, std::memory_order_acq_rel);
}

struct TimedTaker {
  Semaphore* semaphore{nullptr};
  Duration timeout{};
  std::atomic<bool> result{false};
  std::atomic<bool> done{false};
};

void take_with_a_limit(TimedTaker* taker) noexcept {
  taker->result.store(taker->semaphore->try_acquire_for(taker->timeout), std::memory_order_release);
  taker->done.store(true, std::memory_order_release);
}

// One token going around. The impatient threads are the ones that can walk away with a wakeup meant
// for someone else, and the patient ones are what that shows on. They wait without a limit, so
// a token that nobody hears about leaves them asleep for good once the others are gone.
struct TokenRing {
  Semaphore semaphore{0};
  std::uint32_t rounds{0};
  std::atomic<std::uint32_t> running{0};
  std::atomic<std::uint64_t> taken{0};
};

void take_and_give_back(TokenRing* ring) noexcept {
  for (std::uint32_t round = 0; round < ring->rounds; ++round) {
    ring->semaphore.acquire();
    ring->taken.fetch_add(1, std::memory_order_relaxed);
    ring->semaphore.release();
  }

  ring->running.fetch_sub(1, std::memory_order_acq_rel);
}

void keep_giving_up(TokenRing* ring) noexcept {
  for (std::uint32_t round = 0; round < ring->rounds; ++round) {
    if (ring->semaphore.try_acquire_for(Duration::microseconds(1 + round % 64))) {
      ring->semaphore.release();
    }
  }

  ring->running.fetch_sub(1, std::memory_order_acq_rel);
}

hw::timer::TimerAction release_from_interrupt(hw::timer::Timer&, void* semaphore) noexcept {
  static_cast<Semaphore*>(semaphore)->release();
  return hw::timer::TimerAction::Done;
}

// A lock, a condition and a variable to go with it.
struct Mailbox {
  Mutex mutex{};
  CondVar not_empty{};
  CondVar not_full{};
  std::uint32_t letters{0};  // Only under the Mutex.
  std::uint64_t delivered{0};
  std::uint32_t capacity{1};
};

struct Post {
  Mailbox* box{nullptr};
  std::uint32_t letters{0};
  std::atomic<std::uint32_t>* running{nullptr};
};

void deliver(Post* post) noexcept {
  Mailbox& box = *post->box;

  for (std::uint32_t letter = 0; letter < post->letters; ++letter) {
    utils::NakedGuard guard{box.mutex};
    while (box.letters == box.capacity) {
      box.not_full.wait(box.mutex);
    }

    ++box.letters;
    box.not_empty.notify_one();
  }

  post->running->fetch_sub(1, std::memory_order_acq_rel);
}

void collect(Post* post) noexcept {
  Mailbox& box = *post->box;

  for (std::uint32_t letter = 0; letter < post->letters; ++letter) {
    utils::NakedGuard guard{box.mutex};
    while (box.letters == 0) {
      box.not_empty.wait(box.mutex);
    }

    --box.letters;
    ++box.delivered;
    box.not_full.notify_one();
  }

  post->running->fetch_sub(1, std::memory_order_acq_rel);
}

struct Listener {
  Mailbox* box{nullptr};
  Duration timeout{};
  bool use_timeout{false};
  std::atomic<bool> notified{false};
  std::atomic<bool> held_the_lock_after{false};
  std::atomic<bool> done{false};
};

void listen(Listener* listener) noexcept {
  Mailbox& box = *listener->box;
  box.mutex.lock();

  if (listener->use_timeout) {
    listener->notified.store(box.not_empty.wait_for(box.mutex, listener->timeout),
                             std::memory_order_release);
  } else {
    box.not_empty.wait(box.mutex);
    listener->notified.store(true, std::memory_order_release);
  }

  listener->held_the_lock_after.store(box.mutex.owner() == sched::current(),
                                      std::memory_order_release);
  box.mutex.unlock();
  listener->done.store(true, std::memory_order_release);
}

// Takes the lock, and only then goes on to wait, when told. Whoever is in line for the lock by then
// gets it the moment this lets go.
struct LastMoment {
  Mailbox* box{nullptr};
  std::atomic<bool> holding{false};
  std::atomic<bool> go_on{false};
  std::atomic<bool> done{false};
};

void wait_at_the_last_moment(LastMoment* moment) noexcept {
  Mailbox& box = *moment->box;

  box.mutex.lock();
  moment->holding.store(true, std::memory_order_release);
  support::sleep_until_set(moment->go_on);

  box.not_empty.wait(box.mutex);

  box.mutex.unlock();
  moment->done.store(true, std::memory_order_release);
}

void notify_under_the_lock(Mailbox* box) noexcept {
  box->mutex.lock();
  box->not_empty.notify_one();
  box->mutex.unlock();
}

struct Watcher {
  Completion* completion{nullptr};
  std::atomic<std::uint32_t>* done{nullptr};
};

void watch(Watcher* watcher) noexcept {
  watcher->completion->wait();
  watcher->done->fetch_add(1, std::memory_order_acq_rel);
}

// The one thing that a Completion is for: the waiter owns it, on its stack, and goes on with its
// life the moment it is told. The other thread is still in complete_all() at that point.
struct Request {
  std::atomic<Completion*> completion{nullptr};
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> running{0};
};

void serve(Request* request) noexcept {
  while (!request->stop.load(std::memory_order_acquire)) {
    Completion* completion = request->completion.exchange(nullptr, std::memory_order_acq_rel);
    if (completion != nullptr) {
      completion->complete_all();
    } else {
      hw::pause();
    }
  }

  request->running.fetch_sub(1, std::memory_order_acq_rel);
}

hw::timer::TimerAction complete_from_interrupt(hw::timer::Timer&, void* completion) noexcept {
  static_cast<Completion*>(completion)->complete_all();
  return hw::timer::TimerAction::Done;
}

struct Counter {
  Latch* latch{nullptr};
};

void count_down(Counter* counter) noexcept { counter->latch->count_down(); }
}  // namespace

KTEST(Semaphore, counts_its_tokens) {
  Semaphore semaphore{2};

  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{2});
  KEXPECT(semaphore.try_acquire());
  KEXPECT(semaphore.try_acquire());
  KEXPECT(!semaphore.try_acquire());
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{0});

  semaphore.release(3);
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{3});
}

KTEST(Semaphore, acquire_with_a_token_there_does_not_wait) {
  Semaphore semaphore{1};
  semaphore.acquire();
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{0});
}

KTEST(Semaphore, a_release_wakes_one_taker) {
  Semaphore semaphore;
  std::atomic<std::uint32_t> done{0};
  Taker taker{.semaphore = &semaphore, .done = &done};

  for (std::uint32_t i = 0; i < 2; ++i) {
    KREQUIRE(start("sem", &take_one, &taker) != nullptr);
  }

  support::spin_for(A_MOMENT);
  KEXPECT_EQ(done.load(), 0u);

  semaphore.release();
  KEXPECT(wait_for([&] { return done.load() == 1; }));
  support::spin_for(A_MOMENT);
  KEXPECT_EQ(done.load(), 1u);

  semaphore.release();
  KEXPECT(wait_for([&] { return done.load() == 2; }));
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{0});
}

KTEST(Semaphore, releasing_several_wakes_several) {
  Semaphore semaphore;
  std::atomic<std::uint32_t> done{0};
  Taker taker{.semaphore = &semaphore, .done = &done};

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(start("sem", &take_one, &taker) != nullptr);
  }

  support::spin_for(A_MOMENT);
  semaphore.release(THREADS);

  KEXPECT(wait_for([&] { return done.load() == THREADS; }));
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{0});
}

KTEST(Semaphore, try_acquire_for_gives_up) {
  Semaphore semaphore;
  TimedTaker taker{.semaphore = &semaphore, .timeout = Duration::milliseconds(5)};

  KREQUIRE(start("sem", &take_with_a_limit, &taker) != nullptr);
  KEXPECT(wait_for([&] { return taker.done.load(std::memory_order_acquire); }));
  KEXPECT(!taker.result.load());
  KEXPECT_EQ(semaphore.available(), std::ptrdiff_t{0});
}

KTEST(Semaphore, try_acquire_for_gets_a_token_that_shows_up) {
  Semaphore semaphore;
  TimedTaker taker{.semaphore = &semaphore, .timeout = Duration::seconds(60)};

  KREQUIRE(start("sem", &take_with_a_limit, &taker) != nullptr);
  support::spin_for(A_MOMENT);

  semaphore.release();
  KEXPECT(wait_for([&] { return taker.done.load(std::memory_order_acquire); }));
  KEXPECT(taker.result.load());
}

KTEST(Semaphore, an_interrupt_handler_can_release) {
  Semaphore semaphore;
  std::atomic<std::uint32_t> done{0};
  Taker taker{.semaphore = &semaphore, .done = &done};

  KREQUIRE(start("sem", &take_one, &taker) != nullptr);

  hw::timer::Timer timer{&release_from_interrupt, &semaphore};
  (void)timer.start_after(Duration::milliseconds(1));

  KEXPECT(wait_for([&] { return done.load() == 1; }));
  timer.cancel_sync();
}

KTEST(Semaphore, no_token_gets_stranded_by_a_timeout) {
  static constexpr std::uint32_t PATIENT = 4;
  static constexpr std::uint32_t IMPATIENT = 4;

  TokenRing ring;
  ring.rounds = 4000;
  ring.running.store(PATIENT + IMPATIENT, std::memory_order_release);

  for (std::uint32_t i = 0; i < PATIENT; ++i) {
    KREQUIRE(start("ring", &take_and_give_back, &ring) != nullptr);
  }

  for (std::uint32_t i = 0; i < IMPATIENT; ++i) {
    KREQUIRE(start("ring", &keep_giving_up, &ring) != nullptr);
  }

  ring.semaphore.release();

  KEXPECT(wait_for([&] { return ring.running.load(std::memory_order_acquire) == 0; }));
  KEXPECT_EQ(ring.taken.load(), std::uint64_t{PATIENT} * ring.rounds);
  KEXPECT_EQ(ring.semaphore.available(), std::ptrdiff_t{1});
}

KTEST(CondVar, a_notification_with_nobody_there_does_nothing) {
  CondVar condition;

  condition.notify_one();
  condition.notify_all();
  KEXPECT_EQ(condition.waiters(), 0u);
}

KTEST(CondVar, notify_one_wakes_a_waiter) {
  Mailbox box;
  Listener listener{.box = &box};

  KREQUIRE(start("cv", &listen, &listener) != nullptr);
  KREQUIRE(wait_for([&] { return box.not_empty.waiters() == 1; }));
  KEXPECT(!listener.done.load());

  box.not_empty.notify_one();

  KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));
  KEXPECT(listener.held_the_lock_after.load());
}

KTEST(CondVar, wait_lets_go_of_the_mutex) {
  Mailbox box;
  Listener listener{.box = &box};

  KREQUIRE(start("cv", &listen, &listener) != nullptr);
  KREQUIRE(wait_for([&] { return box.not_empty.waiters() == 1; }));

  // It gets on the queue first and lets go second, so the lock can take a moment.
  KREQUIRE(wait_for([&] { return box.mutex.try_lock(); }));
  box.not_empty.notify_one();

  // It has the notification, but cannot go on while the lock is here.
  support::spin_for(A_MOMENT);
  KEXPECT(!listener.done.load());

  box.mutex.unlock();
  KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));
}

// What the whole thing exists for. On one CPU, with the notifier ahead by priority, so that it runs
// the instant the waiter lets go of the lock. If the waiter was not on the queue by then, that
// notification goes nowhere and the waiter sleeps through it for good.
KTEST(CondVar, a_notification_right_after_letting_go_is_not_lost) {
  const std::uint32_t cpu = support::last_cpu();

  Mailbox box;
  LastMoment moment{.box = &box};

  KREQUIRE(start("cv", &wait_at_the_last_moment, &moment, support::pinned_to(cpu)) != nullptr);
  KREQUIRE(wait_for([&] { return moment.holding.load(std::memory_order_acquire); }));

  const Thread* notifier = start("cv-notify", &notify_under_the_lock, &box,
                                 support::of_class(sched::PriorityClass::RealTime, cpu));
  KREQUIRE(notifier != nullptr);
  KREQUIRE(wait_for([&] { return notifier->state() == sched::ThreadState::Blocked; }));

  moment.go_on.store(true, std::memory_order_release);

  KEXPECT(wait_for([&] { return moment.done.load(std::memory_order_acquire); }));
}

KTEST(CondVar, notify_all_wakes_everyone) {
  Mailbox box;
  std::array<Listener, THREADS> listeners;

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    listeners[i].box = &box;
    KREQUIRE(start("cv", &listen, &listeners[i]) != nullptr);
  }

  KREQUIRE(wait_for([&] { return box.not_empty.waiters() == THREADS; }));

  box.not_empty.notify_all();

  for (const Listener& listener : listeners) {
    KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));
    KEXPECT(listener.held_the_lock_after.load());
  }
}

KTEST(CondVar, wait_for_comes_back_with_the_mutex_after_a_timeout) {
  Mailbox box;
  Listener listener{.box = &box, .timeout = Duration::milliseconds(5), .use_timeout = true};

  KREQUIRE(start("cv", &listen, &listener) != nullptr);
  KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));

  KEXPECT(!listener.notified.load());
  KEXPECT(listener.held_the_lock_after.load());
  KEXPECT_EQ(box.not_empty.waiters(), 0u);
}

KTEST(CondVar, wait_for_says_when_a_notification_came) {
  Mailbox box;
  Listener listener{.box = &box, .timeout = Duration::seconds(60), .use_timeout = true};

  KREQUIRE(start("cv", &listen, &listener) != nullptr);
  KREQUIRE(wait_for([&] { return box.not_empty.waiters() == 1; }));

  box.not_empty.notify_one();

  KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));
  KEXPECT(listener.notified.load());
}

KTEST(CondVar, wakes_nobody_without_being_told) {
  Mailbox box;
  Listener listener{.box = &box};

  KREQUIRE(start("cv", &listen, &listener) != nullptr);
  KREQUIRE(wait_for([&] { return box.not_empty.waiters() == 1; }));

  // Preemption and other wakes are the usual sources, and neither may reach the caller.
  support::spin_for(A_MOMENT);
  KEXPECT(!listener.done.load());
  KEXPECT_EQ(box.not_empty.waiters(), 1u);

  box.not_empty.notify_all();
  KEXPECT(wait_for([&] { return listener.done.load(std::memory_order_acquire); }));
}

// Every letter has to be seen exactly once, with the box never overflowing.
KTEST(CondVar, a_bounded_buffer_works) {
  static constexpr std::uint32_t PAIRS = 3;
  static constexpr std::uint32_t LETTERS = 4000;

  Mailbox box;
  box.capacity = 4;

  std::atomic<std::uint32_t> running{PAIRS * 2};
  Post post{.box = &box, .letters = LETTERS, .running = &running};

  for (std::uint32_t i = 0; i < PAIRS; ++i) {
    KREQUIRE(start("post", &deliver, &post) != nullptr);
    KREQUIRE(start("post", &collect, &post) != nullptr);
  }

  KEXPECT(wait_for([&] { return running.load(std::memory_order_acquire) == 0; }));
  KEXPECT_EQ(box.delivered, std::uint64_t{PAIRS} * LETTERS);
  KEXPECT_EQ(box.letters, 0u);
}

KTEST(Completion, waiting_for_what_already_happened_returns_at_once) {
  Completion completion;

  KEXPECT(!completion.is_done());
  completion.complete_all();
  KEXPECT(completion.is_done());

  completion.wait();
  KEXPECT(completion.wait_for(Duration{}));
}

KTEST(Completion, wakes_everyone_who_waits) {
  Completion completion;
  std::atomic<std::uint32_t> done{0};
  Watcher watcher{.completion = &completion, .done = &done};

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(start("cmpl", &watch, &watcher) != nullptr);
  }

  support::spin_for(A_MOMENT);
  KEXPECT_EQ(done.load(), 0u);

  completion.complete_all();
  KEXPECT(wait_for([&] { return done.load() == THREADS; }));
}

KTEST(Completion, wait_for_gives_up) {
  Completion completion;
  KEXPECT(!completion.wait_for(Duration::milliseconds(2)));
}

KTEST(Completion, can_be_used_again_after_a_reset) {
  Completion completion;

  completion.complete_all();
  completion.wait();
  completion.reset();

  KEXPECT(!completion.is_done());
  KEXPECT(!completion.wait_for(Duration::microseconds(100)));
}

KTEST(Completion, an_interrupt_handler_can_complete_it) {
  Completion completion;
  std::atomic<std::uint32_t> done{0};
  Watcher watcher{.completion = &completion, .done = &done};

  KREQUIRE(start("cmpl", &watch, &watcher) != nullptr);

  hw::timer::Timer timer{&complete_from_interrupt, &completion};
  (void)timer.start_after(Duration::milliseconds(1));

  KEXPECT(wait_for([&] { return done.load() == 1; }));
  timer.cancel_sync();
}

// What gets overwritten here is the next round's Completion, on the same stack, so a server that
// is still touching the old one has its lock and queue changed under it.
KTEST(Completion, can_go_away_the_moment_it_is_done) {
  if (hw::smp::online_cpus() < 2) {
    KTEST_SKIP("takes someone on another CPU to still be in there");
  }

  Request request;
  request.running.store(1, std::memory_order_release);
  KREQUIRE(start("serve", &serve, &request, support::pinned_to(support::last_cpu())) != nullptr);

  for (std::uint32_t round = 0; round < 20'000; ++round) {
    Completion completion;
    request.completion.store(&completion, std::memory_order_release);
    completion.wait();
  }

  request.stop.store(true, std::memory_order_release);
  KEXPECT(wait_for([&] { return request.running.load(std::memory_order_acquire) == 0; }));
}

KTEST(Latch, a_latch_at_zero_is_open) {
  Latch latch{0};

  KEXPECT(latch.is_done());
  latch.wait();
}

KTEST(Latch, opens_when_the_count_runs_out) {
  Latch latch{THREADS};
  Counter counter{.latch = &latch};

  KEXPECT(!latch.wait_for(Duration::microseconds(50)));

  for (std::uint32_t i = 0; i < THREADS; ++i) {
    KREQUIRE(start("latch", &count_down, &counter) != nullptr);
  }

  latch.wait();
  KEXPECT(latch.is_done());
}

KTEST(Latch, one_call_can_count_for_several) {
  Latch latch{3};

  latch.count_down(2);
  KEXPECT(!latch.is_done());

  latch.count_down();
  KEXPECT(latch.is_done());
}
}  // namespace kernel::testing
