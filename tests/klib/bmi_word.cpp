#include "bmi_word.hpp"

#include <atomic>
#include <bit>
#include <cstdint>

#include "ktest.hpp"

namespace {
// Shared by the CPUs of the multi-CPU test below.
constinit klib::BmiWord g_word{0};
std::atomic<std::uint64_t> g_seen{0};
std::atomic<std::uint32_t> g_duplicates{0};
}  // namespace

KTEST(BmiWord, starts_all_free) {
  klib::BmiWord word;
  KEXPECT(word.is_empty());
  KEXPECT(!(word.is_full()));
  KEXPECT_EQ(word.free_count(), 64u);
}

KTEST(BmiWord, acquire_first_free_takes_lowest_slots) {
  klib::BmiWord word;
  for (std::uint32_t i = 0; i < 64; ++i) {
    KEXPECT_EQ(word.acquire_first_free(), i);
  }

  KEXPECT(word.is_full());
  KEXPECT_EQ(word.acquire_first_free(), 64u);

  word.release_bit(17);
  KEXPECT_EQ(word.acquire_first_free(), 17u);
}

KTEST(BmiWord, acquire_specific) {
  klib::BmiWord word;
  KEXPECT(word.acquire_specific(5));
  KEXPECT(!(word.acquire_specific(5)));
  KEXPECT_EQ(word.snapshot(), ~(std::uint64_t{1} << 5));
}

KTEST(BmiWord, acquire_contiguous_finds_the_lowest_run) {
  // Free: bits 2-3, 10-14, 40-63.
  klib::BmiWord word{0b11ull << 2 | 0b11111ull << 10 | (~0ull << 40)};

  KEXPECT_EQ(word.acquire_contiguous(2), 2u);
  KEXPECT_EQ(word.acquire_contiguous(5), 10u);
  KEXPECT_EQ(word.acquire_contiguous(3), 40u);
  KEXPECT_EQ(word.acquire_contiguous(21), 43u);
  KEXPECT_EQ(word.acquire_contiguous(1), 64u);
}

KTEST(BmiWord, acquire_contiguous_every_length) {
  for (std::uint32_t count = 1; count <= 64; ++count) {
    klib::BmiWord word;
    KEXPECT_EQ(word.acquire_contiguous(count), 0u);
    KEXPECT_EQ(std::popcount(~word.snapshot()), static_cast<int>(count));

    word.release_contiguous(0, count);
    KEXPECT(word.is_empty());
  }
}

KTEST(BmiWord, acquire_contiguous_skips_short_runs) {
  // Runs of 3 everywhere, separated by one taken slot.
  std::uint64_t mask = 0;
  for (int i = 0; i + 3 <= 64; i += 4) {
    mask |= 0b111ull << i;
  }

  klib::BmiWord word{mask};
  KEXPECT_EQ(word.acquire_contiguous(4), 64u);
  KEXPECT_EQ(word.acquire_contiguous(3), 0u);
  KEXPECT_EQ(word.acquire_contiguous(3), 4u);
}

// Every CPU grabs slots until none are left, a few rounds over: no slot may go to two of them.
KTEST_ALL_CPUS(BmiWord, hands_each_slot_out_once_under_contention, cpu) {
  for (int round = 0; round < 200; ++round) {
    if (cpu == 0) {
      g_word.reset();
      g_seen.store(0, std::memory_order_relaxed);
    }
    kernel::testing::sync_cpus();

    for (std::uint32_t idx = g_word.acquire_first_free(); idx != 64;
         idx = g_word.acquire_first_free()) {
      const std::uint64_t bit = std::uint64_t{1} << idx;
      if ((g_seen.fetch_or(bit, std::memory_order_relaxed) & bit) != 0) {
        g_duplicates.fetch_add(1, std::memory_order_relaxed);
      }
    }
    kernel::testing::sync_cpus();

    if (cpu == 0) {
      KEXPECT_EQ(g_seen.load(), ~0ull);
    }
  }

  KEXPECT_EQ(g_duplicates.load(), 0u);
}
