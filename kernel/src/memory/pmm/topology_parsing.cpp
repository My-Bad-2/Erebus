#include <string.h>
#include <uacpi/acpi.h>

#include "drivers/acpi.hpp"
#include "hal/smp/smp.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
namespace {
// The variable-length arrays of ACPI tables have no alignment guarantee.
template <typename T>
[[nodiscard]] T load_unaligned(const std::uint8_t* src) noexcept {
  T value;
  klib::memcpy(&value, src, sizeof(T));
  return value;
}

[[nodiscard]] bool is_latency(const std::uint8_t data_type) noexcept {
  return data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_LATENCY ||
         data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_LATENCY ||
         data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_LATENCY;
}

[[nodiscard]] bool is_bandwidth(const std::uint8_t data_type) noexcept {
  return data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_BANDWIDTH ||
         data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_BANDWIDTH ||
         data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_BANDWIDTH;
}
}  // namespace

void TopologyParser::parse_srat(const EarlyAllocFn alloc) noexcept {
  using namespace drivers::acpi;
  auto srat_res = Table::find<"SRAT">();
  if (!srat_res.has_value()) {
    return;
  }

  const Table srat = std::move(*srat_res);
  const auto payload_res = srat.payload<acpi_srat>();
  if (!payload_res.has_value()) {
    return;
  }

  const auto enabled_memory = [](const acpi_entry_hdr* hdr) -> const acpi_srat_memory_affinity* {
    if (hdr->type != ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      return nullptr;
    }

    const auto* mem = entry_as<acpi_srat_memory_affinity>(hdr);
    return (mem && (mem->flags & ACPI_SRAT_MEMORY_ENABLED)) ? mem : nullptr;
  };

  for (const acpi_entry_hdr* hdr : *payload_res) {
    if (enabled_memory(hdr)) {
      m_domain_count++;
    }
  }

  if (m_domain_count == 0) {
    return;
  }

  // Collect the proximity domains, then sort and dedupe them: the index of a domain in the
  // resulting array is its dense node ID. The array is kept for SLIT/HMAT lookups.
  auto* acpi_domains =
      alloc(sizeof(std::uint32_t) * m_domain_count, alignof(std::uint32_t)).as<std::uint32_t>();

  std::size_t map_idx = 0;
  for (const acpi_entry_hdr* hdr : *payload_res) {
    if (const auto* mem = enabled_memory(hdr)) {
      acpi_domains[map_idx++] = mem->proximity_domain;
    }
  }

  const auto domain_span = std::span{acpi_domains, m_domain_count};
  std::ranges::sort(domain_span);
  const auto unique_tail = std::ranges::unique(domain_span);

  m_active_nodes =
      static_cast<std::uint32_t>(std::distance(domain_span.begin(), unique_tail.begin()));

  // The per-node tables are quadratic in the node count. A count past the limit is firmware
  // nonsense, and pretending to be one node is the safe way out of it.
  if (m_active_nodes > hw::smp::MAX_NUMA_NODES) [[unlikely]] {
    utils::logger::warn("SRAT lists {} memory domains (limit {}), ignoring NUMA\n", m_active_nodes,
                        hw::smp::MAX_NUMA_NODES);
    m_active_nodes = 0;
    m_domain_count = 0;
    return;
  }

  m_acpi_to_internal_map = acpi_domains;

  m_domains =
      alloc(sizeof(MemoryDomain) * m_domain_count, alignof(MemoryDomain)).as<MemoryDomain>();

  std::size_t domain_idx = 0;
  for (const acpi_entry_hdr* hdr : *payload_res) {
    if (const auto* mem = enabled_memory(hdr)) {
      m_domains[domain_idx++] = {PhysicalAddress{mem->address},
                                 PhysicalAddress{mem->address + mem->length},
                                 get_internal_node_id(mem->proximity_domain)};
    }
  }
}

void TopologyParser::parse_slit(const EarlyAllocFn alloc) noexcept {
  using namespace drivers::acpi;
  auto slit_res = Table::find<"SLIT">();
  if (!slit_res.has_value()) {
    return;
  }

  const Table slit = std::move(*slit_res);
  const auto slit_ptr_res = slit.as<acpi_slit>();
  if (!slit_ptr_res.has_value()) {
    return;
  }

  const auto* slit_ptr = *slit_ptr_res;
  // SLIT is indexed by raw ACPI proximity domain, so its size is unrelated to m_active_nodes.
  const std::uint64_t count = slit_ptr->num_localities;

  std::size_t expected_matrix_size = 0;
  if (__builtin_mul_overflow(count, count, &expected_matrix_size)) {
    utils::logger::warn("SLIT dimensions overflow. Ignoring SLIT.\n");
    return;
  }

  const std::size_t payload_len = slit_ptr->hdr.length - sizeof(acpi_slit);
  if (payload_len < expected_matrix_size) {
    utils::logger::warn("SLIT payload length mismatch. Ignoring SLIT.\n");
    return;
  }

  m_latencies =
      alloc(m_active_nodes * m_active_nodes * sizeof(std::uint32_t), alignof(std::uint32_t))
          .as<std::uint32_t>();

  // Defaults in case SLIT does not cover every pair.
  for (std::uint32_t src = 0; src < m_active_nodes; ++src) {
    for (std::uint32_t tgt = 0; tgt < m_active_nodes; ++tgt) {
      m_latencies[src * m_active_nodes + tgt] = (src == tgt) ? 10 : 20;
    }
  }

  const auto* raw_matrix = reinterpret_cast<const std::uint8_t*>(slit_ptr) + sizeof(acpi_slit);

  for (std::uint32_t src_acpi = 0; src_acpi < count; ++src_acpi) {
    const std::uint32_t src_internal = get_internal_node_id(src_acpi);
    if (src_internal == INVALID_NODE) {
      continue;
    }

    for (std::uint32_t tgt_acpi = 0; tgt_acpi < count; ++tgt_acpi) {
      const std::uint32_t tgt_internal = get_internal_node_id(tgt_acpi);
      if (tgt_internal != INVALID_NODE) {
        m_latencies[src_internal * m_active_nodes + tgt_internal] =
            raw_matrix[src_acpi * count + tgt_acpi];
      }
    }
  }
}

void TopologyParser::process_hmat_locality(const void* slb) const noexcept {
  const auto* loc = static_cast<const acpi_hmat_locality*>(slb);
  const auto* base = static_cast<const std::uint8_t*>(slb);

  const std::uint64_t num_init = loc->num_initiator_proximity_domains;
  const std::uint64_t num_tgt = loc->num_target_proximity_domains;

  // Layout after the fixed header: u32 initiators[num_init], u32 targets[num_tgt],
  // u16 matrix[num_init][num_tgt]. Refuse structures whose arrays do not fit in their length.
  const std::uint64_t initiators_off = sizeof(acpi_hmat_locality);
  const std::uint64_t targets_off = initiators_off + num_init * sizeof(std::uint32_t);
  const std::uint64_t matrix_off = targets_off + num_tgt * sizeof(std::uint32_t);
  const std::uint64_t needed = matrix_off + num_init * num_tgt * sizeof(std::uint16_t);
  if (num_init > 0xFFFF || num_tgt > 0xFFFF || needed > loc->hdr.length) {
    utils::logger::warn("HMAT locality structure is truncated. Ignoring it.\n");
    return;
  }

  const bool latency = is_latency(loc->data_type);
  const bool bandwidth = is_bandwidth(loc->data_type);
  if (!latency && !bandwidth) {
    return;
  }

  const auto initiator = [&](const std::uint64_t i) {
    return load_unaligned<std::uint32_t>(base + initiators_off + i * sizeof(std::uint32_t));
  };
  const auto target = [&](const std::uint64_t t) {
    return load_unaligned<std::uint32_t>(base + targets_off + t * sizeof(std::uint32_t));
  };
  const auto entry = [&](const std::uint64_t i, const std::uint64_t t) {
    return load_unaligned<std::uint16_t>(base + matrix_off +
                                         (i * num_tgt + t) * sizeof(std::uint16_t));
  };

  for (std::uint64_t t = 0; t < num_tgt; ++t) {
    const std::uint32_t target_domain = target(t);
    const std::uint32_t target_internal = get_internal_node_id(target_domain);
    if (target_internal == INVALID_NODE) {
      continue;
    }

    // Only the node's view of its own memory is interesting for tiering.
    for (std::uint64_t i = 0; i < num_init; ++i) {
      if (initiator(i) != target_domain) {
        continue;
      }

      const std::uint64_t raw_val = entry(i, t);
      if (raw_val == 0 || raw_val == 0xFFFF) {
        continue;
      }

      const std::uint64_t scaled_val = raw_val * loc->entry_base_unit;
      NodeMetrics& metrics = m_node_metrics[target_internal];

      if (latency) {
        // Latencies are reported in picoseconds.
        const auto lat_ns =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(scaled_val / 1000));
        metrics.base_latency_ns = lat_ns;

        if (lat_ns < 60) {
          metrics.tier = MemoryTier::HBM;
        } else if (lat_ns < 150) {
          metrics.tier = MemoryTier::DDR;
        } else {
          metrics.tier = MemoryTier::CXL_LOCAL;
        }
      } else {
        // Bandwidths are reported in MB/s.
        metrics.bandwidth_mbps = static_cast<std::uint32_t>(scaled_val);
      }
    }
  }
}

void TopologyParser::parse_hmat() const noexcept {
  using namespace drivers::acpi;
  auto hmat_res = Table::find<"HMAT">();
  if (!hmat_res.has_value()) {
    return;
  }

  const Table hmat = std::move(*hmat_res);
  const auto tbl_res = hmat.as<acpi_hmat>();
  if (!tbl_res.has_value()) {
    return;
  }

  const auto* hmat_ptr = *tbl_res;
  auto current = reinterpret_cast<const std::uint8_t*>(hmat_ptr->entries);
  const auto end = reinterpret_cast<const std::uint8_t*>(hmat_ptr) + hmat_ptr->hdr.length;

  while (current + sizeof(acpi_hmat_entry_hdr) <= end) {
    const auto* hdr = reinterpret_cast<const acpi_hmat_entry_hdr*>(current);
    if (hdr->length < sizeof(acpi_hmat_entry_hdr) || current + hdr->length > end) [[unlikely]] {
      break;
    }

    if (hdr->type == ACPI_HMAT_ENTRY_TYPE_LOCALITY && hdr->length >= sizeof(acpi_hmat_locality)) {
      process_hmat_locality(hdr);
    }

    current += hdr->length;
  }
}

void TopologyParser::parse(const EarlyAllocFn alloc) noexcept {
  parse_srat(alloc);

  // No usable SRAT: treat the machine as a single node.
  if (m_active_nodes == 0) {
    m_active_nodes = 1;
    m_acpi_to_internal_map =
        alloc(sizeof(std::uint32_t), alignof(std::uint32_t)).as<std::uint32_t>();
    m_acpi_to_internal_map[0] = 0;
  }

  m_node_metrics =
      alloc(sizeof(NodeMetrics) * m_active_nodes, alignof(NodeMetrics)).as<NodeMetrics>();
  for (std::uint32_t i = 0; i < m_active_nodes; ++i) {
    m_node_metrics[i] = NodeMetrics{};
  }

  parse_slit(alloc);
  parse_hmat();

  if (m_domains && m_domain_count > 0) {
    const std::span domains_span{m_domains, m_domain_count};

    std::ranges::sort(domains_span,
                      [](const MemoryDomain& a, const MemoryDomain& b) { return a.base < b.base; });

    for (std::size_t i = 0; i + 1 < m_domain_count; ++i) {
      if (domains_span[i].end > domains_span[i + 1].base) {
        utils::logger::fatal("Firmware Bug: Overlapping ACPI SRAT memory domains detected!\n");
      }
    }
  }
}
}  // namespace kernel::memory::pmm
