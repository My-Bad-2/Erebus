#include "memory/pmm/bootstrap.hpp"

namespace kernel::memory::pmm {
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

  std::uint32_t highest_node = 0;

  // Tally the exact requirements
  for (const acpi_entry_hdr *hdr : *payload_res) {
    if (hdr->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      const auto *mem = reinterpret_cast<const acpi_srat_memory_affinity *>(hdr);
      if (mem->flags & ACPI_SRAT_MEMORY_ENABLED) {
        m_domain_count++;
        highest_node = std::max(mem->proximity_domain, highest_node);
      }
    }
  }

  m_active_nodes = highest_node + 1;
  if (m_domain_count == 0) {
    return;
  }

  m_domains = alloc(sizeof(MemoryDomain) * m_domain_count, alignof(MemoryDomain)).as<MemoryDomain>();

  std::size_t idx = 0;
  for (const acpi_entry_hdr *hdr : *payload_res) {
    if (hdr->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
      const auto *mem = reinterpret_cast<const acpi_srat_memory_affinity *>(hdr);
      if (mem->flags & ACPI_SRAT_MEMORY_ENABLED) {
        m_domains[idx++] = {
            PhysicalAddress{mem->address},
            PhysicalAddress{mem->address + mem->length},
            mem->proximity_domain,
        };
      }
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

  const auto *slit_ptr = *slit_ptr_res;
  const std::uint64_t count = slit_ptr->num_localities;

  if (count > m_active_nodes) {
    m_active_nodes = static_cast<std::uint32_t>(count);
  }

  m_latencies =
      alloc(sizeof(std::uint32_t) * m_active_nodes * m_active_nodes, alignof(std::uint32_t)).as<std::uint32_t>();

  const std::uint8_t *raw_matrix = reinterpret_cast<const std::uint8_t *>(slit_ptr) + sizeof(acpi_slit);

  for (std::uint32_t src = 0; src < count; ++src) {
    for (std::uint32_t tgt = 0; tgt < count; ++tgt) {
      m_latencies[src * m_active_nodes + tgt] = raw_matrix[src * count + tgt];
    }
  }
}

void TopologyParser::process_hmat_locality(const void *slb) const noexcept {
  auto sllb = static_cast<const acpi_hmat_locality *>(slb);

  acpi_hmat_locality_arrays arrays;
  uacpi_hmat_locality_get_arrays(const_cast<acpi_hmat_locality *>(sllb), &arrays);

  const auto *initiators = reinterpret_cast<const std::uint32_t *>(arrays.initiator_domains);
  const auto *targets = static_cast<const std::uint32_t *>(arrays.target_domains);
  const auto *matrix = static_cast<const std::uint16_t *>(arrays.matrix);

  const bool is_latency = (sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_LATENCY ||
                           sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_LATENCY ||
                           sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_LATENCY);

  const bool is_bandwidth = (sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_BANDWIDTH ||
                             sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_READ_BANDWIDTH ||
                             sllb->data_type == ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_BANDWIDTH);

  for (std::uint32_t t = 0; t < sllb->num_target_proximity_domains; ++t) {
    const std::uint32_t target_node = targets[t];
    if (target_node >= m_active_nodes) {
      continue;
    }

    for (std::uint32_t i = 0; i < sllb->num_initiator_proximity_domains; ++i) {
      const std::uint32_t init_node = initiators[i];

      if (init_node == target_node) {
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

          m_node_metrics[target_node].base_latency_ns = lat_ns;

          if (lat_ns < 60) {
            m_node_metrics[target_node].tier = MemoryTier::HBM;
          } else if (lat_ns < 150) {
            m_node_metrics[target_node].tier = MemoryTier::DDR;
          } else {
            m_node_metrics[target_node].tier = MemoryTier::CXL_LOCAL;
          }
        } else if (is_bandwidth) {
          m_node_metrics[target_node].bandwidth_mbps = static_cast<std::uint32_t>(scaled_val);
        }
      }
    }
  }
}

void TopologyParser::parse_hmat() noexcept {
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

    switch (hdr->type) {
    case ACPI_HMAT_ENTRY_TYPE_LOCALITY:
      process_hmat_locality(hdr);
      break;
    case ACPI_HMAT_ENTRY_TYPE_PROXIMITY_DOMAIN:
      break;
    case ACPI_HMAT_ENTRY_TYPE_CACHE:
      break;
    default:
      break;
    }

    current += hdr->length;
  }
}

void TopologyParser::parse(EarlyAllocFn &&alloc) noexcept {
  parse_srat(std::move(alloc));
  m_node_metrics = alloc(sizeof(NodeMetrics) * m_active_nodes, alignof(NodeMetrics)).as<NodeMetrics>();

  for (std::uint32_t i = 0; i < m_active_nodes; ++i) {
    m_node_metrics[i] = NodeMetrics{};
  }

  parse_slit(alloc);
  parse_hmat();

  if (m_domains && m_domain_count > 0) {
    std::sort(m_domains, m_domains + m_domain_count,
              [](const MemoryDomain &a, const MemoryDomain &b) { return a.base < b.base; });
  }
}
} // namespace kernel::memory::pmm