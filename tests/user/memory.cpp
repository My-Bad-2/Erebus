// The Memory family from user code: anonymous memory, shared objects, and what the kernel says
// about memory and quotas.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ktest.hpp"
#include "rt/thread.hpp"
#include "support.hpp"
#include "sys/invoker.hpp"
#include "sys/syscalls.hpp"

namespace user::testing {
namespace {
constexpr std::size_t PAGE = 4096;

[[nodiscard]] bool all_zero(const std::span<const std::byte> bytes) noexcept {
  for (const std::byte b : bytes) {
    if (b != std::byte{0}) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] std::span<std::byte> bytes_at(void* const at, const std::size_t size) noexcept {
  return {static_cast<std::byte*>(at), size};
}

// For the race below: where the copier writes, and when to stop.
struct CopyRace {
  void* target;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> copies{0};
};

void copy_until_told(void* const context) noexcept {
  auto& race = *static_cast<CopyRace*>(context);
  while (!race.stop.load(std::memory_order_relaxed)) {
    // MemInfo writes 40 bytes to wherever it is told. While the page is not there, it fails.
    std::uint64_t rax = std::to_underlying(sys::Syscall::MemInfo);
    register void* rdi asm("rdi") = race.target;
    asm volatile("syscall" : "+a"(rax) : "r"(rdi) : "rcx", "r11", "memory");
    race.copies.fetch_add(1, std::memory_order_relaxed);
  }
}
}  // namespace

KTEST(Memory, allocate_gives_zeroed_memory_that_can_be_written) {
  constexpr std::size_t SIZE = 3 * PAGE;
  const auto memory = sys::mem::allocate(SIZE);
  KREQUIRE_OK(memory);

  const std::span<std::byte> bytes = bytes_at(*memory, SIZE);
  KEXPECT(all_zero(bytes));
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(i * 13);
  }
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    KREQUIRE_EQ(bytes[i], static_cast<std::byte>(i * 13));
  }

  KEXPECT_OK(sys::mem::unmap(*memory));
  KEXPECT_ERROR(sys::mem::unmap(*memory), sys::Error::NotMapped);
}

KTEST(Memory, populated_memory_is_there_at_once) {
  const auto memory = sys::mem::allocate(
      PAGE, sys::AccessFlags::ReadWrite | sys::AccessFlags::Populate, sys::PageSize::Default);
  KREQUIRE_OK(memory);
  KEXPECT(all_zero(bytes_at(*memory, PAGE)));
  KEXPECT_OK(sys::mem::unmap(*memory));
}

KTEST(Memory, allocate_refuses_what_it_does_not_know) {
  // 0x8 is the kernel's own User bit, which no caller gets to pass.
  KEXPECT_ERROR(sys::mem::allocate(PAGE, static_cast<sys::AccessFlags>(0x8)),
                sys::Error::InvalidFlags);
  KEXPECT_ERROR(
      sys::mem::allocate(PAGE, sys::AccessFlags::ReadWrite, static_cast<sys::PageSize>(9)),
      sys::Error::InvalidArgument);
  KEXPECT_ERROR(sys::mem::allocate(PAGE, sys::AccessFlags::ReadWrite, sys::PageSize::Size4K,
                                   reinterpret_cast<void*>(0xFFFF'8000'0000'0000)),
                sys::Error::InvalidArgument);
}

KTEST(Memory, a_hint_on_memory_already_there_is_refused) {
  const auto memory = sys::mem::allocate(PAGE);
  KREQUIRE_OK(memory);
  KEXPECT_ERROR(
      sys::mem::allocate(PAGE, sys::AccessFlags::ReadWrite, sys::PageSize::Size4K, *memory),
      sys::Error::AlreadyMapped);
  KEXPECT_OK(sys::mem::unmap(*memory));

  // Free again, so the same hint is fine.
  const auto again =
      sys::mem::allocate(PAGE, sys::AccessFlags::ReadWrite, sys::PageSize::Size4K, *memory);
  KEXPECT_OK(again);
  KEXPECT_EQ(again.value_or(nullptr), *memory);
  if (again) {
    KEXPECT_OK(sys::mem::unmap(*again));
  }
}

// Write goes, and comes back. A child that writes in between faults.
KTEST(Memory, protect_takes_permissions_away_and_gives_them_back) {
  const auto memory = sys::mem::allocate(PAGE);
  KREQUIRE_OK(memory);
  auto* const word = static_cast<volatile std::uint64_t*>(*memory);
  *word = 1;

  KREQUIRE_OK(sys::mem::protect(*memory, PAGE, sys::AccessFlags::Read));
  KEXPECT_EQ(*word, 1u);

  const auto status = in_child("read-only", [word] {
    *word = 2;
    return 0;
  });
  KREQUIRE_OK(status);
  KEXPECT_EQ(*status, -1);  // proc::EXIT_FAULT

  KEXPECT_OK(sys::mem::protect(*memory, PAGE, sys::AccessFlags::ReadWrite));
  *word = 3;
  KEXPECT_EQ(*word, 3u);

  KEXPECT_ERROR(sys::mem::protect(*memory, PAGE, static_cast<sys::AccessFlags>(0x1000)),
                sys::Error::InvalidFlags);
  KEXPECT_OK(sys::mem::unmap(*memory));
}

KTEST(Memory, info_adds_up) {
  const auto info = sys::mem::info();
  KREQUIRE_OK(info);
  KEXPECT_GT(info->total_bytes, 0u);
  KEXPECT_LE(info->free_bytes, info->total_bytes);
  KEXPECT_GT(info->quota_limit, 0u);
  KEXPECT_LE(info->quota_used, info->quota_limit);
  KEXPECT_LE(info->quota_available, info->quota_limit - info->quota_used);

  KEXPECT_ERROR(sys::SyscallInvoker::invoke_as<void>(
                    sys::Syscall::MemInfo, reinterpret_cast<void*>(0xFFFF'8000'0000'0000)),
                sys::Error::InvalidArgument);
}

// Anonymous memory is a strict commitment: all of it counts from the start, and asking for more
// than there is room for is refused there and then.
//
// What earlier tests made is freed in the background, a grace period after they let go of it,
// and its charge comes back whenever that is. So the quota is never quite still: the numbers here
// leave a margin far larger than anything a test frees, and far smaller than what they measure.
KTEST(Memory, allocations_count_against_the_quota) {
  constexpr std::size_t SIZE = 16 << 20;
  constexpr std::size_t MARGIN = 1 << 20;
  const auto before = sys::mem::info();
  KREQUIRE_OK(before);

  // Not touched, so nothing but the commitment can account for the difference.
  const auto memory = sys::mem::allocate(SIZE);
  KREQUIRE_OK(memory);
  const auto during = sys::mem::info();
  KREQUIRE_OK(during);
  KEXPECT_GE(during->quota_used + MARGIN, before->quota_used + SIZE);
  KEXPECT_OK(sys::mem::unmap(*memory));

  // Measured again, as the unmap gave the charge back.
  const auto after = sys::mem::info();
  KREQUIRE_OK(after);
  const std::size_t too_much = (after->quota_available & ~(PAGE - 1)) + 64 * MARGIN;
  const auto refused = sys::mem::allocate(too_much);
  KEXPECT_ERROR(refused, sys::Error::QuotaExceeded);
  if (refused) {
    (void)sys::mem::unmap(*refused);  // so that the tests after this one still have a quota
  }
}

KTEST(Memory, a_shared_object_shows_the_same_pages_wherever_it_is_mapped) {
  const auto object = sys::cap::create_vmo(2 * PAGE);
  KREQUIRE_OK(object);

  const auto first = sys::mem::map_object(*object);
  const auto second = sys::mem::map_object(*object);
  KREQUIRE_OK(first);
  KREQUIRE_OK(second);
  KEXPECT_NE(*first, *second);

  auto* const one = static_cast<volatile std::uint64_t*>(*first);
  auto* const other = static_cast<volatile std::uint64_t*>(*second);
  one[0] = 0x1111;
  one[PAGE / sizeof(std::uint64_t)] = 0x2222;
  KEXPECT_EQ(other[0], 0x1111u);
  KEXPECT_EQ(other[PAGE / sizeof(std::uint64_t)], 0x2222u);

  // A slice of the second page alone, read-only.
  const auto slice = sys::mem::derive_object(*object, 1, 1, sys::CapRights::Read);
  KREQUIRE_OK(slice);
  KEXPECT_ERROR(sys::mem::map_object(*slice), sys::Error::AccessDenied);
  const auto view = sys::mem::map_object(*slice, sys::AccessFlags::Read);
  KREQUIRE_OK(view);
  KEXPECT_EQ(*static_cast<volatile std::uint64_t*>(*view), 0x2222u);
  KEXPECT_ERROR(sys::mem::derive_object(*object, 2, 1), sys::Error::OutOfBounds);

  // Physical addresses, for devices: page-aligned, and not for a page past the end. They take
  // Manage, which mapping does not: neither the read-only slice nor a copy that may only map the
  // object gets them.
  const auto physical = sys::mem::translate_object(*object, 0);
  KEXPECT_OK(physical);
  KEXPECT_EQ(physical.value_or(1) % PAGE, 0u);
  KEXPECT_ERROR(sys::mem::translate_object(*object, 2), sys::Error::OutOfBounds);
  KEXPECT_ERROR(sys::mem::translate_object(*slice, 0), sys::Error::AccessDenied);
  const auto mapper =
      sys::cap::copy(sys::NULL_CAP, *object, sys::NULL_CAP, sys::CapRights::ReadWrite);
  KREQUIRE_OK(mapper);
  KEXPECT_ERROR(sys::mem::translate_object(*mapper, 0), sys::Error::AccessDenied);
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *mapper));

  // It only grows.
  KEXPECT_ERROR(sys::mem::resize_object(*object, 1), sys::Error::InvalidArgument);
  KEXPECT_OK(sys::mem::resize_object(*object, 3));
  KEXPECT_OK(sys::mem::translate_object(*object, 2));

  KEXPECT_OK(sys::mem::unmap(*view));
  KEXPECT_OK(sys::mem::unmap(*first));
  KEXPECT_OK(sys::mem::unmap(*second));
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *slice));
  KEXPECT_OK(sys::cap::remove(sys::NULL_CAP, *object));
}

// The kernel copies a call's results to user memory while other threads of the process may
// unmap it. A page freed under a copy that had already found it would get the copy after it went
// back to the allocator: here, a page this thread got since, somewhere else, would not be zero.
//
// The freed page most likely comes straight back as the next new one, from this CPU's cache. A
// late copy lands whenever the copier gets round to it, so each new page is kept for a few rounds
// before it is looked at, and the last ones once the copier is gone and every copy it started has
// landed. Each round yields, so that on one CPU the two take turns: the copier is preemptible in
// the kernel, so a copy can be cut off halfway there as well.
KTEST(Memory, a_page_unmapped_under_a_copy_is_never_written_after_it_is_gone) {
  constexpr int ROUNDS = 400;
  constexpr std::size_t KEPT = 8;
  // On one CPU the copier only gets turns as the scheduler sees fit: after a slice of its own it
  // waits until this thread has had as much, yield or not. So the rounds go on until it did make
  // this many copies meanwhile, as long as that takes, up to a limit.
  constexpr std::uint64_t MIN_COPIES = 50;
  constexpr std::uint64_t LIMIT_NS = 2'000'000'000;
  constexpr auto POPULATED = sys::AccessFlags::ReadWrite | sys::AccessFlags::Populate;

  const auto first = sys::mem::allocate(PAGE, POPULATED);
  KREQUIRE_OK(first);
  void* const target = *first;

  CopyRace race{.target = target};
  auto copier = rt::Thread::spawn("copier", &copy_until_told, &race);
  KREQUIRE_OK(copier);
  while (race.copies.load() == 0) {
    sys::thread::yield();
  }
  const std::uint64_t copies_before = race.copies.load();

  int dirty = 0;
  std::array<void*, KEPT> kept{};
  const auto look_at = [&dirty](void*& page) {
    if (page != nullptr) {
      dirty += all_zero(bytes_at(page, PAGE)) ? 0 : 1;
      (void)sys::mem::unmap(page);
      page = nullptr;
    }
  };

  const std::uint64_t give_up = sys::time::now() + LIMIT_NS;
  const auto enough = [&](const int round) {
    return round >= ROUNDS &&
           (race.copies.load() - copies_before >= MIN_COPIES || sys::time::now() >= give_up);
  };

  for (int round = 0; !enough(round); ++round) {
    (void)sys::mem::unmap(target);

    const auto fresh = sys::mem::allocate(PAGE, POPULATED);
    if (!fresh) {
      break;
    }
    void*& slot = kept[static_cast<std::size_t>(round) % KEPT];
    look_at(slot);
    slot = *fresh;

    if (!sys::mem::allocate(PAGE, POPULATED, sys::PageSize::Size4K, target)) {
      break;
    }
    sys::thread::yield();
  }

  const std::uint64_t copies_during = race.copies.load() - copies_before;
  race.stop.store(true);
  KEXPECT_OK(copier->join());
  for (void*& page : kept) {
    look_at(page);
  }
  (void)sys::mem::unmap(target);

  KEXPECT_EQ(dirty, 0);
  KEXPECT_GE(copies_during, MIN_COPIES);  // or the race was hardly run
}
}  // namespace user::testing
