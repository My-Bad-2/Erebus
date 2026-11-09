// Page faults against the writers of the same address space. Faults take no lock while no writer
// is about, and writers wait for the faults that got in before they change anything; these would
// show a writer that went ahead anyway. The faults are driven by hand, as in fork.cpp: these page
// maps are never loaded into CR3.
#include <atomic>
#include <chrono.hpp>
#include <cstdint>
#include <optional>

#include "../sched/support.hpp"
#include "core/errors.hpp"
#include "drivers/clock.hpp"
#include "hal/hal.hpp"
#include "ktest.hpp"
#include "memory/memory.hpp"
#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"
#include "utils/rcu/rcu.hpp"

namespace kernel::testing {
namespace {
using klib::Duration;
using memory::DirectMap;
using memory::PAGE_SIZE;
using memory::VirtualAddress;
using memory::vmm::AccessFlags;
using memory::vmm::AddressSpace;
using memory::vmm::has_flag;

constexpr auto RW = AccessFlags::Read | AccessFlags::Write;
constexpr VirtualAddress USER_BASE{0x0000100000000000ul};
constexpr Duration RUN = Duration::milliseconds(300);

[[nodiscard]] VirtualAddress page_of(const VirtualAddress base, const std::size_t i) noexcept {
  return base + static_cast<VirtualAddress::OffsetType>(i * PAGE_SIZE);
}

// A writer on CPU 0 and readers everywhere else, in rounds. Each round CPU 0 makes a fresh range,
// lets the readers loose on it, does whatever it does to it while they fault its pages in, then
// waits for them to be through before it looks.
struct Rounds {
  std::atomic<std::uint64_t> base{0};
  std::atomic<std::uint64_t> round{0};
  std::atomic<std::uint32_t> done{0};
  std::atomic<bool> stop{false};

  void reset() noexcept {
    base.store(0);
    round.store(0);
    done.store(0);
    stop.store(false);
  }

  void start(const std::uint64_t number, const VirtualAddress range) noexcept {
    base.store(range.value(), std::memory_order_relaxed);
    done.store(0, std::memory_order_relaxed);
    round.store(number, std::memory_order_release);
  }

  void wait_for(const std::uint32_t readers) const noexcept {
    while (done.load(std::memory_order_acquire) != readers) {
      hw::pause();
    }
  }

  // Reader: the range of the next round, or nothing once CPU 0 has stopped.
  [[nodiscard]] VirtualAddress next(std::uint64_t& last) const noexcept {
    while (round.load(std::memory_order_acquire) == last) {
      if (stop.load(std::memory_order_acquire)) {
        return VirtualAddress{};
      }

      hw::pause();
    }

    last = round.load(std::memory_order_acquire);
    return VirtualAddress{base.load(std::memory_order_relaxed)};
  }
};

// Read faults on every page of @p pages at @p base, in an order of its own. Counts the pages that
// came in writable and those that came in read-only, and the faults that failed.
struct Faulted {
  std::uint64_t writable{0};
  std::uint64_t read_only{0};
  std::uint64_t failed{0};
};

void read_fault_all(AddressSpace& space, const VirtualAddress base, const std::size_t pages,
                    Rng& rng, Faulted& out) noexcept {
  // Any odd stride visits every page of a power of two once.
  const std::size_t first = rng.next() % pages;
  const std::size_t stride = (rng.next() % (pages / 2)) * 2 + 1;
  for (std::size_t k = 0; k < pages; ++k) {
    const VirtualAddress page = page_of(base, (first + k * stride) % pages);
    if (!space.handle_page_fault(page, AccessFlags::Read)) {
      ++out.failed;
      continue;
    }

    const auto found = space.pagemap().translate(page);
    if (found && has_flag(found->flags, AccessFlags::Write)) {
      ++out.writable;
    } else {
      ++out.read_only;
    }
  }
}
}  // namespace

// A fault that read a VMA's flags before protect() changed them must not map its page afterwards
// the way they were: once protect() is back, nothing in the range may be writable. Some faults
// come before it, and map their pages writable for protect() to take back; some after.
KTEST_ALL_CPUS(ConcurrentFaults, nothing_in_a_protected_range_is_left_writable, cpu) {
  constexpr std::size_t PAGES = 64;
  constexpr std::size_t EDGE = 8;  // the range protected is [EDGE, PAGES - EDGE)
  static AddressSpace* space = nullptr;
  static Rounds rounds;
  static std::atomic<std::uint64_t> left_writable{0};
  static std::atomic<std::uint64_t> came_writable{0};
  static std::atomic<std::uint64_t> came_read_only{0};
  static std::atomic<std::uint64_t> failed{0};

  if (cpu == 0) {
    space = new AddressSpace(USER_BASE);
    rounds.reset();
    left_writable.store(0);
    came_writable.store(0);
    came_read_only.store(0);
    failed.store(0);
  }

  sync_cpus();

  const std::uint32_t readers = cpu_count() - 1;
  Rng rng{0x9F + cpu};
  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t round = 0;
    while (drivers::SystemClock::now() < end) {
      const auto range = space->alloc(PAGES * PAGE_SIZE, RW);
      if (!range.has_value()) {
        KEXPECT(range.has_value());
        break;
      }

      rounds.start(++round, *range);
      support::spin_for(Duration::microseconds(static_cast<std::int64_t>(rng.next() % 40)));

      const VirtualAddress middle = page_of(*range, EDGE);
      constexpr std::size_t MIDDLE_BYTES = (PAGES - 2 * EDGE) * PAGE_SIZE;
      KEXPECT(space->protect(middle, MIDDLE_BYTES, AccessFlags::Read).has_value());

      if (readers == 0) {
        Faulted alone;
        read_fault_all(*space, *range, PAGES, rng, alone);
        failed.fetch_add(alone.failed, std::memory_order_relaxed);
      }

      rounds.wait_for(readers);
      for (std::size_t i = EDGE; i < PAGES - EDGE; ++i) {
        const auto found = space->pagemap().translate(page_of(*range, i));
        if (found && has_flag(found->flags, AccessFlags::Write)) {
          left_writable.fetch_add(1, std::memory_order_relaxed);
        }
      }

      KEXPECT(space->protect(middle, MIDDLE_BYTES, RW).has_value());
      KEXPECT(space->free(*range).has_value());
    }

    rounds.stop.store(true, std::memory_order_release);
  } else {
    std::uint64_t last = 0;
    Faulted mine;
    for (VirtualAddress range = rounds.next(last); !range.is_null(); range = rounds.next(last)) {
      read_fault_all(*space, range, PAGES, rng, mine);
      rounds.done.fetch_add(1, std::memory_order_release);
    }

    came_writable.fetch_add(mine.writable, std::memory_order_relaxed);
    came_read_only.fetch_add(mine.read_only, std::memory_order_relaxed);
    failed.fetch_add(mine.failed, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(left_writable.load(), 0u);
    KEXPECT_EQ(failed.load(), 0u);
    if (readers != 0) {
      KEXPECT_GT(came_writable.load(), 0u);
      KEXPECT_GT(came_read_only.load(), 0u);
    }

    utils::logger::info("Faults: {} rounds of protect(), {} pages came writable, {} read-only\n",
                        rounds.round.load(), came_writable.load(), came_read_only.load());
    delete space;
  }

  sync_cpus();
}

// The same with a fork: once clone_into() is back, every page of the parent is copy-on-write,
// read-only until written. A fault that mapped one writable after the copy would let the child
// see what the parent writes from then on.
KTEST_ALL_CPUS(ConcurrentFaults, a_fork_leaves_every_page_of_the_parent_copy_on_write, cpu) {
  constexpr std::size_t PAGES = 64;
  static AddressSpace* space = nullptr;
  static Rounds rounds;
  static std::atomic<std::uint64_t> left_writable{0};
  static std::atomic<std::uint64_t> came_writable{0};
  static std::atomic<std::uint64_t> failed{0};

  if (cpu == 0) {
    space = new AddressSpace(USER_BASE);
    rounds.reset();
    left_writable.store(0);
    came_writable.store(0);
    failed.store(0);
  }

  sync_cpus();

  const std::uint32_t readers = cpu_count() - 1;
  Rng rng{0x4F + cpu};
  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t round = 0;
    while (drivers::SystemClock::now() < end) {
      const auto range = space->alloc(PAGES * PAGE_SIZE, RW);
      if (!range.has_value()) {
        KEXPECT(range.has_value());
        break;
      }

      rounds.start(++round, *range);
      support::spin_for(Duration::microseconds(static_cast<std::int64_t>(rng.next() % 40)));

      auto* child = new AddressSpace(USER_BASE);
      KEXPECT(space->clone_into(*child).has_value());

      if (readers == 0) {
        Faulted alone;
        read_fault_all(*space, *range, PAGES, rng, alone);
        failed.fetch_add(alone.failed, std::memory_order_relaxed);
      }

      rounds.wait_for(readers);
      for (std::size_t i = 0; i < PAGES; ++i) {
        const auto found = space->pagemap().translate(page_of(*range, i));
        if (found && has_flag(found->flags, AccessFlags::Write)) {
          left_writable.fetch_add(1, std::memory_order_relaxed);
        }
      }

      delete child;
      KEXPECT(space->free(*range).has_value());
    }

    rounds.stop.store(true, std::memory_order_release);
  } else {
    std::uint64_t last = 0;
    Faulted mine;
    for (VirtualAddress range = rounds.next(last); !range.is_null(); range = rounds.next(last)) {
      read_fault_all(*space, range, PAGES, rng, mine);
      rounds.done.fetch_add(1, std::memory_order_release);
    }

    came_writable.fetch_add(mine.writable, std::memory_order_relaxed);
    failed.fetch_add(mine.failed, std::memory_order_relaxed);
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(left_writable.load(), 0u);
    KEXPECT_EQ(failed.load(), 0u);
    if (readers != 0) {
      KEXPECT_GT(came_writable.load(), 0u);
    }

    utils::logger::info("Faults: {} forks, {} pages came writable before theirs\n",
                        rounds.round.load(), came_writable.load());
    delete space;
  }

  sync_cpus();
}

// Every CPU makes ranges of its own in one address space, faults every page in, writes a word to
// it and reads all of them back before it frees the range, while CPU 0 also splits and merges
// VMAs of its own there. Lookups run alongside all of it; one that found the wrong VMA, or a page
// mapped twice, would come back with someone else's word.
KTEST_ALL_CPUS(ConcurrentFaults, faults_and_writers_in_one_space_keep_every_page_its_own, cpu) {
  constexpr std::size_t PAGES = 128;
  static AddressSpace* space = nullptr;
  static std::atomic<bool> stop{false};
  static std::atomic<std::uint64_t> faults{0};
  static std::atomic<std::uint64_t> wrong{0};
  static std::atomic<std::uint64_t> failed{0};
  static std::atomic<std::uint64_t> changes{0};

  if (cpu == 0) {
    space = new AddressSpace(USER_BASE);
    stop.store(false);
    faults.store(0);
    wrong.store(0);
    failed.store(0);
    changes.store(0);
  }

  sync_cpus();

  const auto one_pass = [&](const std::uint64_t pass) noexcept {
    const auto range = space->alloc(PAGES * PAGE_SIZE, RW);
    if (!range.has_value()) {
      failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto tag = [&](const std::size_t i) {
      return (std::uint64_t{cpu} << 48) | (pass << 16) | i;
    };

    for (std::size_t i = 0; i < PAGES; ++i) {
      const VirtualAddress page = page_of(*range, i);
      const auto found = space->handle_page_fault(page, AccessFlags::Write).has_value()
                             ? space->pagemap().translate(page)
                             : std::nullopt;
      if (!found) {
        failed.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      *DirectMap::phys_to_virt(found->phys_address).as<volatile std::uint64_t>() = tag(i);
    }

    for (std::size_t i = 0; i < PAGES; ++i) {
      const auto found = space->pagemap().translate(page_of(*range, i));
      if (found &&
          *DirectMap::phys_to_virt(found->phys_address).as<volatile std::uint64_t>() != tag(i)) {
        wrong.fetch_add(1, std::memory_order_relaxed);
      }
    }

    faults.fetch_add(PAGES, std::memory_order_relaxed);
    if (!space->free(*range).has_value()) {
      failed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  if (cpu == 0) {
    const klib::TimePoint end = drivers::SystemClock::now() + RUN;
    std::uint64_t pass = 0;
    while (drivers::SystemClock::now() < end) {
      // A range cut in three and put together again: the tree changes twice.
      const auto scratch = space->alloc(16 * PAGE_SIZE, RW);
      if (scratch.has_value()) {
        KEXPECT(space->protect(page_of(*scratch, 4), 8 * PAGE_SIZE, AccessFlags::Read).has_value());
        KEXPECT(space->protect(page_of(*scratch, 4), 8 * PAGE_SIZE, RW).has_value());
        KEXPECT(space->free(*scratch).has_value());
        changes.fetch_add(4, std::memory_order_relaxed);
      }

      if (++pass % 8 == 0 || cpu_count() == 1) {
        one_pass(pass);
      }
    }

    stop.store(true, std::memory_order_release);
  } else {
    for (std::uint64_t pass = 0; !stop.load(std::memory_order_acquire); ++pass) {
      one_pass(pass);
    }
  }

  sync_cpus();

  if (cpu == 0) {
    KEXPECT_EQ(wrong.load(), 0u);
    KEXPECT_EQ(failed.load(), 0u);
    KEXPECT_GT(faults.load(), 0u);
    utils::logger::info("Faults: {} faults checked, against {} changes to VMAs\n", faults.load(),
                        changes.load());
    delete space;
  }

  sync_cpus();
}

// Growing a stack is a writer, and a writer waits for read sections to end, so from inside one it
// would wait for itself. It says so instead, and grows as usual from outside.
KTEST(ConcurrentFaults, a_stack_does_not_grow_from_inside_a_read_section) {
  auto* space = new AddressSpace(USER_BASE);
  const auto stack = space->alloc(4 * PAGE_SIZE, RW | AccessFlags::Stack);
  KREQUIRE(stack.has_value());

  const VirtualAddress below = *stack - static_cast<VirtualAddress::OffsetType>(PAGE_SIZE);
  {
    utils::rcu::ReadGuard rcu;
    const auto grown = space->handle_page_fault(below, AccessFlags::Write);
    KEXPECT(!grown.has_value() && grown.error() == Error::DeadlockDetected);
  }

  KEXPECT(space->handle_page_fault(below, AccessFlags::Write).has_value());
  KEXPECT(space->pagemap().translate(below).has_value());
  delete space;
}
}  // namespace kernel::testing
