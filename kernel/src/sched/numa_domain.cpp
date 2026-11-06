#include "sched/numa_domain.hpp"

#include <algorithm>
#include <memory>

#include "memory/address.hpp"
#include "memory/memory.hpp"
#include "memory/pmm.hpp"
#include "utils/assert.hpp"
#include "utils/maths.hpp"

namespace kernel::sched {
namespace {
// Makes room for @p count objects of type T at @p offset and returns where they start.
template <typename T>
[[nodiscard]] std::size_t reserve(std::size_t& offset, const std::size_t count) noexcept {
  const std::size_t start = utils::maths::align_up(offset, alignof(T));
  offset = start + sizeof(T) * count;
  return start;
}
}  // namespace

auto NumaDomain::initialize(const std::uint32_t node_id, const std::span<const CpuPlacement> cpus,
                            const std::span<const std::uint32_t> neighbors) noexcept
    -> std::expected<void, Error> {
  const auto cpu_count = static_cast<std::uint32_t>(cpus.size());
  const std::uint32_t words = utils::maths::div_round_up(cpu_count, BITS_PER_WORD);

  std::uint32_t core_count = 0;
  for (std::uint32_t slot = 0; slot < cpu_count; ++slot) {
    core_count += slot == 0 || cpus[slot].core != cpus[slot - 1].core;
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
    if (slot == 0 || cpus[slot].core != cpus[slot - 1].core) {
      std::construct_at(&m_cores[cores_seen++], 0u, slot, 0u);
    }

    Core& core = m_cores[cores_seen - 1];
    m_slots[slot] = {.cpu = cpus[slot].cpu, .core = cores_seen - 1};
    core.busy.fetch_add(1, std::memory_order_relaxed);
    ++core.siblings;
  }

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

void NumaDomain::mark_idle(const std::uint32_t slot) noexcept {
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
void NumaDomain::note_busy(const std::uint32_t slot) noexcept {
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

std::uint32_t NumaDomain::claim_idle_core() noexcept {
  for (std::uint32_t word = 0; word < m_words; ++word) {
    while (true) {
      const std::uint32_t bit = m_idle_cores[word].acquire_first_free();
      if (bit == BITS_PER_WORD) {
        break;
      }

      const Core& core = m_cores[word * BITS_PER_WORD + bit];
      for (std::uint32_t sibling = 0; sibling < core.siblings; ++sibling) {
        if (claim_cpu(core.first_slot + sibling)) {
          return cpu_at(core.first_slot + sibling);
        }
      }
    }
  }

  return NO_CPU;
}

std::uint32_t NumaDomain::claim_idle_cpu() noexcept {
  for (std::uint32_t word = 0; word < m_words; ++word) {
    const std::uint32_t bit = m_idle_cpus[word].acquire_first_free();
    if (bit != BITS_PER_WORD) {
      const std::uint32_t slot = word * BITS_PER_WORD + bit;
      note_busy(slot);
      return cpu_at(slot);
    }
  }

  return NO_CPU;
}

std::uint32_t NumaDomain::idle_cpu_count() const noexcept {
  std::uint32_t count = 0;
  for (std::uint32_t word = 0; word < m_words; ++word) {
    count += m_idle_cpus[word].free_count();
  }

  return count;
}

void NumaDomain::set_overloaded(const std::uint32_t slot, const bool overloaded) noexcept {
  klib::BmiWord& word = m_overloaded[word_of(slot)];
  if (overloaded) {
    word.release_bit(bit_of(slot));
  } else {
    (void)word.acquire_specific(bit_of(slot));
  }
}
}  // namespace kernel::sched
