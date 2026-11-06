#include "patch/static_call.hpp"

#include <atomic>
#include <span>

#include "memory/vmm/vma/vma.hpp"
#include "utils/logger.hpp"

extern "C" {
[[gnu::visibility("hidden")]] extern const std::uint8_t __text_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __text_end[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __static_call_tramps_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __static_call_tramps_end[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __static_call_table_start[];
[[gnu::visibility("hidden")]] extern const std::uint8_t __static_call_table_end[];
}

namespace kernel::patch {
namespace {
// The two shapes a trampoline can have, read as one little-endian word: `jmp rel32; int3 x3`,
// and `ret; nopl 0(%rax); int3 x3`.
constexpr std::uint64_t JMP_REL32 = 0xE9;
constexpr std::uint64_t INT3_PADDING = 0xCC'CCCCul << 40;
constexpr std::uint64_t RETURN_TRAMPOLINE = 0xCC'CCCC'0040'1F0F'C3ul;
constexpr std::size_t TRAMPOLINE_ALIGN = 16;

[[nodiscard]] constexpr bool is_trampoline(const std::uint64_t word) noexcept {
  return word == RETURN_TRAMPOLINE || ((word & 0xFF) == JMP_REL32 && (word >> 40) == 0xCC'CCCC);
}

[[nodiscard]] constexpr std::uint64_t jump_trampoline(const std::int32_t rel) noexcept {
  return JMP_REL32 | (std::uint64_t{static_cast<std::uint32_t>(rel)} << 8) | INT3_PADDING;
}

[[nodiscard]] bool in_range(const std::uintptr_t addr, const std::uint8_t* start,
                            const std::uint8_t* end) noexcept {
  return addr >= reinterpret_cast<std::uintptr_t>(start) &&
         addr < reinterpret_cast<std::uintptr_t>(end);
}

// The trampoline word the entry asks for, or why it cannot have it.
std::expected<std::uint64_t, Error> encode(const StaticCallEntry& entry) noexcept {
  const std::uintptr_t site = entry.site;
  if (!in_range(site, __static_call_tramps_start, __static_call_tramps_end) ||
      site % TRAMPOLINE_ALIGN != 0) [[unlikely]] {
    return std::unexpected(Error::SiteOutOfTextBounds);
  }

  if (entry.target == 0) {
    return RETURN_TRAMPOLINE;
  }

  // A target among the trampolines would make the outcome depend on the order of patching.
  if (!in_range(entry.target, __text_start, __text_end) ||
      in_range(entry.target, __static_call_tramps_start, __static_call_tramps_end)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  // The displacement counts from the end of the 5-byte jump. The kernel spans far less than
  // 2 GiB, so it always fits; check anyway rather than jump somewhere else.
  const auto rel = static_cast<std::int64_t>(entry.target - (site + 5));
  if (rel != static_cast<std::int32_t>(rel)) [[unlikely]] {
    return std::unexpected(Error::OutOfBounds);
  }

  return jump_trampoline(static_cast<std::int32_t>(rel));
}

// The entry that should decide the trampoline at `site`, if any applies to this CPU.
const StaticCallEntry* best_entry(const std::span<const StaticCallEntry> entries,
                                  const std::uintptr_t site, const hw::CpuInfo& cpu) noexcept {
  const StaticCallEntry* best = nullptr;
  for (const StaticCallEntry& entry : entries) {
    if (entry.site == site && entry.requirements.is_met_by(cpu) &&
        (best == nullptr || entry.priority >= best->priority)) {
      best = &entry;
    }
  }

  return best;
}

// The CPU is the only one running and has just rewritten code through a different mapping;
// make sure it does not execute stale instructions it may already have fetched.
void serialize(const hw::CpuInfo& cpu) noexcept {
  if (cpu.has(hw::Feature::SERIALIZE)) {
    asm volatile("serialize" ::: "memory");
  } else {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)::"memory");
  }
}
}  // namespace

std::expected<void, Error> StaticCallPatcher::apply_patches() noexcept {
  using namespace memory;

  // Two linker symbols, not one array: subtract them as integers.
  const std::size_t table_bytes = reinterpret_cast<std::uintptr_t>(__static_call_table_end) -
                                  reinterpret_cast<std::uintptr_t>(__static_call_table_start);
  if (table_bytes % sizeof(StaticCallEntry) != 0) [[unlikely]] {
    return std::unexpected(Error::InvalidState);
  }

  const std::span entries{reinterpret_cast<const StaticCallEntry*>(__static_call_table_start),
                          table_bytes / sizeof(StaticCallEntry)};
  if (entries.empty()) {
    return {};
  }

  // The trampolines share a few pages of .text; a writable alias of just those is enough.
  const auto tramps_start = VirtualAddress{__static_call_tramps_start}.align_down(PAGE_SIZE);
  const auto tramps_end = VirtualAddress{__static_call_tramps_end}.align_up(PAGE_SIZE);
  vmm::AddressSpace* kspace = vmm::kernel_space();
  const auto alias = kspace->create_alias(tramps_start, tramps_end - tramps_start,
                                          vmm::AccessFlags::Read | vmm::AccessFlags::Write);
  if (!alias) {
    return std::unexpected(alias.error());
  }

  const hw::CpuInfo& cpu = *hw::profile_manager.get_current();
  std::expected<void, Error> status{};
  std::size_t sites = 0;
  std::size_t patched = 0;

  for (std::size_t i = 0; i < entries.size() && status; ++i) {
    // Each trampoline is decided once, when its first entry comes up.
    const std::uintptr_t site = entries[i].site;
    bool seen = false;
    for (std::size_t prev = 0; prev < i && !seen; ++prev) {
      seen = entries[prev].site == site;
    }

    if (seen) {
      continue;
    }

    ++sites;
    const StaticCallEntry* entry = best_entry(entries.subspan(i), site, cpu);
    if (entry == nullptr) {
      continue;
    }

    const auto word = encode(*entry);
    if (!word) {
      status = std::unexpected(word.error());
      break;
    }

    auto* const current = reinterpret_cast<const std::uint64_t*>(site);
    if (!is_trampoline(*current)) [[unlikely]] {
      status = std::unexpected(Error::UnexpectedSiteMnemonic);
      break;
    }

    // One aligned store: whoever runs the trampoline meanwhile sees the old jump or the new one.
    auto* const writable = (*alias + (VirtualAddress{site} - tramps_start)).as<std::uint64_t>();
    std::atomic_ref{*writable}.store(*word, std::memory_order_relaxed);
    ++patched;
  }

  serialize(cpu);
  (void)kspace->destroy_alias(*alias);

  if (status) {
    utils::logger::info("Static calls: {} of {} retargeted for this CPU.\n", patched, sites);
  }

  return status;
}
}  // namespace kernel::patch
