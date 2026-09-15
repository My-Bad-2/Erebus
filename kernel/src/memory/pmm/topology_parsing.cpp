#include "drivers/acpi.hpp"
#include "memory/pmm/bootstrap.hpp"
#include "utils/logger.hpp"

namespace kernel::memory::pmm {
void TopologyParser::parse_srat(EarlyAllocFn alloc) noexcept {
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

  // Count memory entries
  for (const acpi_entry_hdr *hdr : *payload_res) {
    if (hdr->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      if (reinterpret_cast<const acpi_srat_memory_affinity *>(hdr)->flags & ACPI_SRAT_MEMORY_ENABLED) {
        m_domain_count++;
      }
    }
  }

  if (m_domain_count == 0) {
    return;
  }

  // Extract unique ACPI proximity domains for dense mapping
  auto *temp_domains = alloc(sizeof(std::uint32_t) * m_domain_count, alignof(std::uint32_t)).as<std::uint32_t>();

  std::size_t map_idx = 0;
  for (const acpi_entry_hdr *hdr : *payload_res) {
    if (hdr->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      const auto *mem = reinterpret_cast<const acpi_srat_memory_affinity *>(hdr);
      if (mem->flags & ACPI_SRAT_MEMORY_ENABLED) {
        temp_domains[map_idx++] = mem->proximity_domain;
      }
    }
  }

  // Sort and remove duplicates to create the dense mapping array
  auto domain_span = std::span{temp_domains, m_domain_count};
  std::ranges::sort(domain_span);
  auto unique_subrange = std::ranges::unique(domain_span);

  m_active_nodes = std::distance(domain_span.begin(), unique_subrange.begin());
  m_acpi_to_internal_map = temp_domains; // Keep for SLIT/HMAT translations

  // Map physical memory domains using the new dense IDs
  m_domains = alloc(sizeof(MemoryDomain) * m_domain_count, alignof(MemoryDomain)).as<MemoryDomain>();

  std::size_t domain_idx = 0;
  for (const acpi_entry_hdr *hdr : *payload_res) {
    if (hdr->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      const auto *mem = reinterpret_cast<const acpi_srat_memory_affinity *>(hdr);

      if (mem->flags & ACPI_SRAT_MEMORY_ENABLED) {
        m_domains[domain_idx++] = {PhysicalAddress{mem->address}, PhysicalAddress{mem->address + mem->length},
                                   get_internal_node_id(mem->proximity_domain)};
      }
    }
  }
}

void TopologyParser::parse_slit(EarlyAllocFn alloc) noexcept {
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

  const auto *slit_ptr = *slit_ptr_res;
  const std::uint64_t count = slit_ptr->num_localities; // 'count' equates to max ACPI domain ID

  // Validate matrix dimensions against the physical table length.
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

  // Allocate a dense latency matrix bounded purely by actual nodes, not the ACPI max ID
  m_latencies =
      alloc(m_active_nodes * m_active_nodes * sizeof(std::uint32_t), alignof(std::uint32_t)).as<std::uint32_t>();

  // Initialize with defaults in case SLIT is sparse
  for (std::uint32_t i = 0; i < m_active_nodes * m_active_nodes; ++i) {
    m_latencies[i] = 20;
  }

  for (std::uint32_t i = 0; i < m_active_nodes; ++i) {
    m_latencies[i * m_active_nodes + i] = 10;
  }

  const std::uint8_t *raw_matrix = reinterpret_cast<const std::uint8_t *>(slit_ptr) + sizeof(acpi_slit);

  for (std::uint32_t src_acpi = 0; src_acpi < count; ++src_acpi) {
    for (std::uint32_t tgt_acpi = 0; tgt_acpi < count; ++tgt_acpi) {
      std::uint32_t src_internal = get_internal_node_id(src_acpi);
      std::uint32_t tgt_internal = get_internal_node_id(tgt_acpi);

      // Only import latencies for nodes that actually have memory attached
      if (src_internal != 0xFFFFFFFF && tgt_internal != 0xFFFFFFFF) {
        m_latencies[src_internal * m_active_nodes + tgt_internal] = raw_matrix[src_acpi * count + tgt_acpi];
      }
    }
  }
}

void TopologyParser::process_hmat_locality(const void *slb) const noexcept {
  const auto *sllb = static_cast<const acpi_hmat_locality *>(slb);

  acpi_hmat_locality_arrays arrays;
  uacpi_hmat_locality_get_arrays(const_cast<acpi_hmat_locality *>(sllb), &arrays);

  const auto *initiators = static_cast<const std::uint32_t *>(arrays.initiator_domains);
  const auto *targets = static_cast<const std::uint32_t *>(arrays.target_domains);
  const auto *matrix = static_cast<const std::uint16_t *>(arrays.matrix);

  using namespace drivers::acpi;
  const bool is_latency = (sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_LATENCY ||
                           sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_LATENCY ||
                           sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_LATENCY);

  const bool is_bandwidth = (sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_BANDWIDTH ||
                             sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_BANDWIDTH ||
                             sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_BANDWIDTH);

  for (std::uint32_t t = 0; t < sllb->num_target_proximity_domains; ++t) {
    const std::uint32_t target_internal = get_internal_node_id(targets[t]);
    if (target_internal == 0xFFFFFFFF) {
      continue;
    }

    for (std::uint32_t i = 0; i < sllb->num_initiator_proximity_domains; ++i) {
      if (initiators[i] != targets[t]) {
        continue;
      }

      const std::uint64_t raw_val = matrix[i * sllb->num_target_proximity_domains + t];
      if (raw_val == 0 || raw_val == 0xFFFF) {
        continue;
      }

      const std::uint64_t scaled_val = raw_val * sllb->entry_base_unit;

      if (is_latency) {
        std::uint32_t lat_ns = static_cast<std::uint32_t>(scaled_val / 1000);
        if (lat_ns == 0) {
          lat_ns = static_cast<std::uint32_t>(scaled_val);
        }

        m_node_metrics[target_internal].base_latency_ns = lat_ns;

        if (lat_ns < 60) {
          m_node_metrics[target_internal].tier = MemoryTier::HBM;
        } else if (lat_ns < 150) {
          m_node_metrics[target_internal].tier = MemoryTier::DDR;
        } else {
          m_node_metrics[target_internal].tier = MemoryTier::CXL_LOCAL;
        }
      } else if (is_bandwidth) {
        m_node_metrics[target_internal].bandwidth_mbps = static_cast<std::uint32_t>(scaled_val);
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

  const auto *hmat_ptr = *tbl_res;
  auto current = reinterpret_cast<const std::uint8_t *>(hmat_ptr->entries);
  const auto end = reinterpret_cast<const std::uint8_t *>(hmat_ptr) + hmat_ptr->hdr.length;

  while (current + sizeof(acpi_hmat_entry_hdr) <= end) {
    const auto *hdr = reinterpret_cast<const acpi_hmat_entry_hdr *>(current);
    if (hdr->length == 0 || current + hdr->length > end) [[unlikely]] {
      break;
    }

    if (hdr->type == ACPI_HMAT_ENTRY_TYPE_LOCALITY) {
      process_hmat_locality(hdr);
    }

    current += hdr->length;
  }
}

void TopologyParser::parse(EarlyAllocFn &&alloc) noexcept {
  parse_srat(alloc);

  // Fallback to UMA if SRAT is missing or empty
  if (m_active_nodes == 0) {
    m_active_nodes = 1;
    m_acpi_to_internal_map = alloc(sizeof(std::uint32_t), alignof(std::uint32_t)).as<std::uint32_t>();
    m_acpi_to_internal_map[0] = 0;
  }

  m_node_metrics = alloc(sizeof(NodeMetrics) * m_active_nodes, alignof(NodeMetrics)).as<NodeMetrics>();
  for (std::uint32_t i = 0; i < m_active_nodes; ++i) {
    m_node_metrics[i] = NodeMetrics{};
  }

  parse_slit(alloc);
  parse_hmat();

  if (m_domains && m_domain_count > 0) {
    std::span domains_span{m_domains, m_domain_count};

    std::ranges::sort(domains_span, [](const MemoryDomain &a, const MemoryDomain &b) { return a.base < b.base; });

    // Overlapping SRAT memory domains
    for (std::size_t i = 0; i < m_domain_count - 1; ++i) {
      if (domains_span[i].end > domains_span[i + 1].base) {
        utils::logger::fatal("Firmware Bug: Overlapping ACPI SRAT memory domains detected!\n");
      }
    }
  }
}
} // namespace kernel::memory::pmm