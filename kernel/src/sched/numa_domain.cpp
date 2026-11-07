#include "sched/numa_domain.hpp"

#include <algorithm>
#include <array>
#include <memory>

#include "memory/address.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "sched/balance.hpp"
#include "utils/assert.hpp"
#include "utils/maths.hpp"

namespace kernel::sched {
namespace {
/// Makes room for @p count objects of type T at @p offset and returns where they start.
template <typename T>
[[nodiscard]] std::size_t reserve(std::size_t& offset, const std::size_t count) noexcept {
  const std::size_t start = utils::maths::align_up(offset, alignof(T));
  offset = start + sizeof(T) * count;
  return start;
}

// Two CPUs are in the same group of a level if everything from there on up is the same. Comparing
// the rest as well means that IDs only have to be unique within what is above them.
[[nodiscard]] bool share_group(const CpuPlacement& a, const CpuPlacement& b,
                               const std::uint32_t level) noexcept {
  const std::array first{a.core, a.cluster, a.cache, a.package};
  const std::array second{b.core, b.cluster, b.cache, b.package};

  for (std::uint32_t above = level; above < SHARING_LEVELS; ++above) {
    if (first[above] != second[above]) {
      return false;
    }
  }

  return true;
}
}  // namespace

auto NumaDomain::initialize(const std::uint32_t node_id, const std::span<const CpuPlacement> cpus,
                            const std::span<const std::uint32_t> neighbors) noexcept
    -> std::expected<void, Error> {
  const auto cpu_count = static_cast<std::uint32_t>(cpus.size());
  const std::uint32_t words = utils::maths::div_round_up(cpu_count, BITS_PER_WORD);

  std::uint32_t core_count = 0;
  for (std::uint32_t slot = 0; slot < cpu_count; ++slot) {
    core_count += slot == 0 || !share_group(cpus[slot - 1], cpus[slot], 0);
  }

  // One block for everything: it is all used by the same CPUs, and it never goes away.
  std::size_t offset = 0;
  const std::size_t slots_at = reserve<Slot>(offset, cpu_count);
  const std::size_t cores_at = reserve<Core>(offset, core_count);
  const std::size_t bitmaps_at = reserve<klib::BmiWord>(offset, words * 3);
  const std::size_t neighbors_at = reserve<std::uint32_t>(offset, neighbors.size());

  const std::size_t pages =
      utils::maths::div_round_up(std::max(offset, std::size_t{1}), memory::PAGE_SIZE);
  const auto order = static_cast<std::uint8_t>(std::bit_width(pages - 1));

  const auto block =
      memory::pmm::alloc_pages_on_node(node_id, memory::pmm::PageMobility::Unmovable, order);
  if (!block) {
    return std::unexpected(Error::OutOfMemory);
  }

  auto* base = memory::DirectMap::phys_to_virt(*block).as<std::byte>();

  m_node_id = node_id;
  m_cpu_count = cpu_count;
  m_core_count = core_count;
  m_words = words;
  m_neighbor_count = static_cast<std::uint32_t>(neighbors.size());
  m_storage = base;
  m_storage_order = order;

  m_slots = std::launder(reinterpret_cast<Slot*>(base + slots_at));
  m_cores = std::launder(reinterpret_cast<Core*>(base + cores_at));
  m_idle_cpus = std::launder(reinterpret_cast<klib::BmiWord*>(base + bitmaps_at));
  m_idle_cores = m_idle_cpus + words;
  m_overloaded = m_idle_cores + words;

  auto* neighbor_table = std::launder(reinterpret_cast<std::uint32_t*>(base + neighbors_at));
  std::ranges::copy(neighbors, neighbor_table);
  m_neighbors = neighbor_table;

  std::uninitialized_default_construct_n(m_idle_cpus, words * 3);
  for (std::uint32_t word = 0; word < words * 3; ++word) {
    m_idle_cpus[word].reset(0);
  }

  std::uint32_t cores_seen = 0;
  for (std::uint32_t slot = 0; slot < cpu_count; ++slot) {
    if (slot == 0 || !share_group(cpus[slot - 1], cpus[slot], 0)) {
      std::construct_at(&m_cores[cores_seen++], 0u, slot, 0u);
    }

    Core& core = m_cores[cores_seen - 1];
    m_slots[slot] = {
        .cpu = cpus[slot].cpu, .core = cores_seen - 1, .capacity = FULL_CAPACITY, .groups = {}};
    core.busy.fetch_add(1, std::memory_order_relaxed);
    ++core.siblings;
  }

  for (std::uint32_t level = 0; level < SHARING_LEVELS; ++level) {
    std::uint32_t first = 0;

    for (std::uint32_t slot = 1; slot <= cpu_count; ++slot) {
      if (slot == cpu_count || !share_group(cpus[slot - 1], cpus[slot], level)) {
        for (std::uint32_t member = first; member < slot; ++member) {
          m_slots[member].groups[level] = {first, slot - first};
        }

        first = slot;
      }
    }
  }

  m_top_capacity = FULL_CAPACITY;
  m_mixed_capacity = false;
  return {};
}

void NumaDomain::release() noexcept {
  if (m_storage == nullptr) {
    return;
  }

  const auto phys = memory::DirectMap::virt_to_phys(memory::VirtualAddress{m_storage});
  KASSERT(phys.has_value());
  memory::pmm::free_pages(*phys, static_cast<std::uint8_t>(m_storage_order));

  m_storage = nullptr;
  m_cpu_count = 0;
  m_core_count = 0;
  m_words = 0;
  m_neighbor_count = 0;
}

void NumaDomain::mark_idle(const std::uint32_t slot) const noexcept {
  klib::BmiWord& word = m_idle_cpus[word_of(slot)];
  const std::uint64_t bit = std::uint64_t{1} << bit_of(slot);

  // The idle loop comes by here on every pass. Only this CPU ever sets its bit, so looking first
  // is safe, and it keeps a CPU that is already listed off the shared cache line.
  if ((word.snapshot() & bit) != 0) {
    return;
  }

  word.release_bit(bit_of(slot));

  const std::uint32_t core_index = m_slots[slot].core;
  if (m_cores[core_index].busy.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    m_idle_cores[word_of(core_index)].release_bit(bit_of(core_index));
  }
}

// Goes with clearing the CPU's idle bit, and whoever clears it does this, exactly once. That keeps
// the count right however the CPU left the list. The core bit may end up a little behind if two
// siblings change at the same moment, which claim_idle_core() can live with.
void NumaDomain::note_busy(const std::uint32_t slot) const noexcept {
  const std::uint32_t core_index = m_slots[slot].core;
  if (m_cores[core_index].busy.fetch_add(1, std::memory_order_acq_rel) == 0) {
    (void)m_idle_cores[word_of(core_index)].acquire_specific(bit_of(core_index));
  }
}

bool NumaDomain::claim_cpu(const std::uint32_t slot) noexcept {
  if (!m_idle_cpus[word_of(slot)].acquire_specific(bit_of(slot))) {
    return false;
  }

  note_busy(slot);
  return true;
}

std::uint32_t NumaDomain::claim_idle_core_in(const SlotSpan span, const bool fast_only) noexcept {
  const std::uint32_t first_core = m_slots[span.first].core;
  const std::uint32_t end_core = m_slots[span.first + span.count - 1].core + 1;
  std::uint32_t claimed = NO_CPU;

  (void)for_each_bit(m_idle_cores, first_core, end_core, [&](const std::uint32_t core_index) {
    const Core& core = m_cores[core_index];
    if (fast_only && !is_fast(core.first_slot)) {
      return false;
    }

    // Only a hint, and it may be out of date by now, so whatever it says, the siblings decide.
    if (!m_idle_cores[word_of(core_index)].acquire_specific(bit_of(core_index))) {
      return false;
    }

    for (std::uint32_t sibling = 0; sibling < core.siblings; ++sibling) {
      if (claim_cpu(core.first_slot + sibling)) {
        claimed = cpu_at(core.first_slot + sibling);
        return true;
      }
    }

    return false;
  });

  return claimed;
}

std::uint32_t NumaDomain::claim_idle_cpu_in(const SlotSpan span, const bool fast_only) noexcept {
  std::uint32_t claimed = NO_CPU;

  (void)for_each_bit(m_idle_cpus, span.first, span.first + span.count,
                     [&](const std::uint32_t slot) {
                       if ((fast_only && !is_fast(slot)) || !claim_cpu(slot)) {
                         return false;
                       }

                       claimed = cpu_at(slot);
                       return true;
                     });

  return claimed;
}

std::uint32_t NumaDomain::claim_idle_core() noexcept {
  if (m_cpu_count == 0) {
    return NO_CPU;
  }

  if (m_mixed_capacity) {
    if (const std::uint32_t cpu = claim_idle_core_in(whole_node(), true); cpu != NO_CPU) {
      return cpu;
    }
  }

  return claim_idle_core_in(whole_node(), false);
}

std::uint32_t NumaDomain::claim_idle_cpu() noexcept {
  if (m_cpu_count == 0) {
    return NO_CPU;
  }

  if (m_mixed_capacity) {
    if (const std::uint32_t cpu = claim_idle_cpu_in(whole_node(), true); cpu != NO_CPU) {
      return cpu;
    }
  }

  return claim_idle_cpu_in(whole_node(), false);
}

std::uint32_t NumaDomain::claim_near(const std::uint32_t slot) noexcept {
  const SlotSpan span = group_of(slot, Sharing::Cache);

  // On a hybrid part this is the whole node, so here is where the fast CPUs get their chance.
  if (m_mixed_capacity) {
    if (const std::uint32_t cpu = claim_idle_core_in(span, true); cpu != NO_CPU) {
      return cpu;
    }

    if (const std::uint32_t cpu = claim_idle_cpu_in(span, true); cpu != NO_CPU) {
      return cpu;
    }
  }

  const std::uint32_t cpu = claim_idle_core_in(span, false);
  return cpu != NO_CPU ? cpu : claim_idle_cpu_in(span, false);
}

void NumaDomain::set_capacity(const std::uint32_t slot, const std::uint32_t capacity) noexcept {
  m_slots[slot].capacity = capacity;

  m_top_capacity = 0;
  m_mixed_capacity = false;

  for (std::uint32_t other = 0; other < m_cpu_count; ++other) {
    const std::uint32_t theirs = m_slots[other].capacity;
    m_mixed_capacity |= other != 0 && theirs != m_top_capacity;
    m_top_capacity = std::max(m_top_capacity, theirs);
  }
}

std::uint32_t NumaDomain::idle_cpu_count() const noexcept {
  std::uint32_t count = 0;
  for (std::uint32_t word = 0; word < m_words; ++word) {
    count += m_idle_cpus[word].free_count();
  }

  return count;
}

void NumaDomain::set_overloaded(const std::uint32_t slot, const bool overloaded) const noexcept {
  klib::BmiWord& word = m_overloaded[word_of(slot)];
  if (overloaded) {
    word.release_bit(bit_of(slot));
  } else {
    (void)word.acquire_specific(bit_of(slot));
  }
}
}  // namespace kernel::sched
