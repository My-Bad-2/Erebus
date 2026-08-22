#pragma once

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

#include <expected>
#include <iterator>
#include <span>
#include <type_traits>
#include <utility>

#include "parser.hpp"

// temporary until the PR is merged
UACPI_PACKED(struct acpi_hmat_entry_hdr {
  uacpi_u16 type;
  uacpi_u16 rsvd;
  uacpi_u32 length;
})
UACPI_EXPECT_SIZEOF(struct acpi_hmat_entry_hdr, 8);

// acpi_hmat_entry_hdr->type
enum acpi_hmat_entry_type {
  ACPI_HMAT_ENTRY_TYPE_PROXIMITY_DOMAIN = 0,
  ACPI_HMAT_ENTRY_TYPE_LOCALITY = 1,
  ACPI_HMAT_ENTRY_TYPE_CACHE = 2,
};

UACPI_PACKED(struct acpi_hmat {
  struct acpi_sdt_hdr hdr;
  uacpi_u32 rsvd;
  struct acpi_hmat_entry_hdr entries[];
})
UACPI_EXPECT_SIZEOF(struct acpi_hmat, 40);

// acpi_hmat_proximity_domain->flags
#define ACPI_HMAT_PROXIMITY_DOMAIN_INITIATOR_VALID (1 << 0)
#define ACPI_HMAT_PROXIMITY_DOMAIN_PROCESSOR_VALID_DEPRECATED (1 << 0)

UACPI_PACKED(struct acpi_hmat_proximity_domain {
  struct acpi_hmat_entry_hdr hdr;
  uacpi_u16 flags;
  uacpi_u16 rsvd1;
  uacpi_u32 initiator_proximity_domain;
  uacpi_u32 memory_proximity_domain;
  uacpi_u32 rsvd2;
  union {
    uacpi_u64 start_address_deprecated; // deprecated in ACPI 6.3
    uacpi_u64 rsvd3;
  };
  union {
    uacpi_u64 range_deprecated; // deprecated in ACPI 6.3
    uacpi_u64 rsvd4;
  };
})
UACPI_EXPECT_SIZEOF(struct acpi_hmat_proximity_domain, 40);

// acpi_hmat_locality->flags
#define ACPI_HMAT_LOCALITY_MEMORY_HIERARCHY_DEPRECATED_MASK 0x0F
#define ACPI_HMAT_LOCALITY_AUTOTRANSITIONS_VALID (1 << 4)

// acpi_hmat_locality->data_type
#define ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_LATENCY 0x00
#define ACPI_HMAT_LOCALITY_DATA_TYPE_READ_LATENCY 0x01
#define ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_LATENCY 0x02
#define ACPI_HMAT_LOCALITY_DATA_TYPE_ACCESS_BANDWIDTH 0x03
#define ACPI_HMAT_LOCALITY_DATA_TYPE_READ_BANDWIDTH 0x04
#define ACPI_HMAT_LOCALITY_DATA_TYPE_WRITE_BANDWIDTH 0x05

UACPI_PACKED(struct acpi_hmat_locality {
  struct acpi_hmat_entry_hdr hdr;
  uacpi_u8 flags;
  uacpi_u8 data_type;
  uacpi_u16 min_transfer_size;
  uacpi_u32 num_initiator_proximity_domains;
  uacpi_u32 num_target_proximity_domains;
  uacpi_u32 rsvd1;
  uacpi_u64 entry_base_unit;
})
UACPI_EXPECT_SIZEOF(struct acpi_hmat_locality, 32);

// acpi_hmat_cache->cache_attributes
#define ACPI_HMAT_CACHE_TOTAL_LEVELS_MASK 0x0000000F
#define ACPI_HMAT_CACHE_LEVEL_MASK 0x000000F0
#define ACPI_HMAT_CACHE_LEVEL_SHIFT 4

#define ACPI_HMAT_CACHE_ASSOCIATIVITY_MASK 0x00000F00
#define ACPI_HMAT_CACHE_ASSOCIATIVITY_SHIFT 8
#define ACPI_HMAT_CACHE_ASSOCIATIVITY_NONE 0x00
#define ACPI_HMAT_CACHE_ASSOCIATIVITY_DIRECT 0x01
#define ACPI_HMAT_CACHE_ASSOCIATIVITY_COMPLEX 0x02

#define ACPI_HMAT_CACHE_WRITE_POLICY_MASK 0x0000F000
#define ACPI_HMAT_CACHE_WRITE_POLICY_SHIFT 12
#define ACPI_HMAT_CACHE_WRITE_POLICY_NONE 0x00
#define ACPI_HMAT_CACHE_WRITE_POLICY_WB 0x01
#define ACPI_HMAT_CACHE_WRITE_POLICY_WT 0x02

#define ACPI_HMAT_CACHE_LINE_SIZE_MASK 0xFFFF0000
#define ACPI_HMAT_CACHE_LINE_SIZE_SHIFT 16

UACPI_PACKED(struct acpi_hmat_cache {
  struct acpi_hmat_entry_hdr hdr;
  uacpi_u32 memory_proximity_domain;
  uacpi_u32 rsvd1;
  uacpi_u64 cache_size;
  uacpi_u32 cache_attributes;
  uacpi_u16 rsvd2;
  uacpi_u16 num_smbios_handles;
  uacpi_u16 smbios_handles[];
})
UACPI_EXPECT_SIZEOF(struct acpi_hmat_cache, 32);

struct acpi_hmat_locality_arrays {
  uacpi_u32 *initiator_domains; // Initiator Proximity Domain lists
  uacpi_u32 *target_domains;    // Target Proximity Domain lists
  uacpi_u16 *matrix;            // Matrix of latency/bandwidth values
};

static inline void uacpi_hmat_locality_get_arrays(struct acpi_hmat_locality *loc,
                                                  struct acpi_hmat_locality_arrays *out_arrays) {
  uacpi_u8 *ptr = (uacpi_u8 *)(loc + 1);

  out_arrays->initiator_domains = (uacpi_u32 *)ptr;

  ptr += (loc->num_initiator_proximity_domains * sizeof(uacpi_u32));
  out_arrays->target_domains = (uacpi_u32 *)ptr;

  ptr += (loc->num_target_proximity_domains * sizeof(uacpi_u32));
  out_arrays->matrix = (uacpi_u16 *)ptr;
}

static inline uacpi_u32 *uacpi_hmat_locality_get_initiator_domains(struct acpi_hmat_locality *loc) {
  return (uacpi_u32 *)(loc + 1);
}

static inline uacpi_u32 *uacpi_hmat_locality_get_target_domains(struct acpi_hmat_locality *loc) {
  uacpi_u8 *ptr = (uacpi_u8 *)(loc + 1);
  return (uacpi_u32 *)(ptr + (loc->num_initiator_proximity_domains * sizeof(uacpi_u32)));
}

static inline uacpi_u16 *uacpi_hmat_locality_get_matrix(struct acpi_hmat_locality *loc) {
  uacpi_u8 *ptr = (uacpi_u8 *)(loc + 1);
  ptr += (loc->num_initiator_proximity_domains * sizeof(uacpi_u32));
  ptr += (loc->num_target_proximity_domains * sizeof(uacpi_u32));
  return (uacpi_u16 *)ptr;
}

namespace kernel::drivers::acpi {
class SubtableView {
  const std::uint8_t *m_begin{nullptr};
  const std::uint8_t *m_end{nullptr};

public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = const acpi_entry_hdr *;
  using difference_type = std::ptrdiff_t;

  SubtableView(const std::uint8_t *begin, const std::uint8_t *end) noexcept : m_begin(begin), m_end(end) {}

  class Iterator {
    const std::uint8_t *m_current;
    const std::uint8_t *m_end;

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const acpi_entry_hdr *;
    using difference_type = std::ptrdiff_t;

    Iterator(const std::uint8_t *curr, const std::uint8_t *end) noexcept : m_current(curr), m_end(end) {}

    value_type operator*() const noexcept { return reinterpret_cast<value_type>(m_current); }

    Iterator &operator++() noexcept {
      auto *hdr = reinterpret_cast<value_type>(m_current);
      if (hdr->length == 0) [[unlikely]] {
        m_current = m_end;
        return *this;
      }

      m_current += hdr->length;
      if (m_current > m_end) [[unlikely]] {
        m_current = m_end;
      }

      return *this;
    }

    bool operator!=(const Iterator &other) const noexcept { return m_current < other.m_current; }
  };

  [[nodiscard]] Iterator begin() const noexcept { return Iterator(m_begin, m_end); }

  [[nodiscard]] Iterator end() const noexcept { return Iterator(m_end, m_end); }
};

class Table {
  uacpi_table *m_table{nullptr};

  explicit Table(uacpi_table *tbl) noexcept : m_table(tbl) {}

public:
  Table() = default;

  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;

  Table(Table &&other) noexcept : m_table(std::exchange(other.m_table, nullptr)) {}

  Table &operator=(Table &&other) noexcept {
    if (this != &other) {
      release();
      m_table = std::exchange(other.m_table, nullptr);
    }

    return *this;
  }

  ~Table() { release(); }

  void release() noexcept {
    if (m_table) [[likely]] {
      uacpi_table_unref(m_table);
      m_table = nullptr;
    }
  }

  template <Signature Sig> [[nodiscard]] static std::expected<Table, uacpi_status> find() noexcept {
    uacpi_table out_table{};
    Status st = uacpi_table_find_by_signature(Sig.data, &out_table);

    if (st != UACPI_STATUS_OK) [[unlikely]] {
      return std::unexpected(st);
    }

    return Table{&out_table};
  }

  template <AcpiStruct T> [[nodiscard]] std::expected<const T *, uacpi_status> as() const noexcept {
    if (!m_table || !m_table->hdr) [[unlikely]] {
      return std::unexpected(UACPI_STATUS_NOT_FOUND);
    }

    if (m_table->hdr->length < sizeof(T)) [[unlikely]] {
      return std::unexpected(UACPI_STATUS_INVALID_ARGUMENT);
    }

    return reinterpret_cast<const T *>(m_table->hdr);
  }

  template <AcpiStruct T> [[nodiscard]] std::expected<SubtableView, uacpi_status> payload() const noexcept {
    return as<T>().transform([this](const T *base_struct) {
      const auto *start = reinterpret_cast<const std::uint8_t *>(base_struct) + sizeof(T);
      const auto *end = reinterpret_cast<const std::uint8_t *>(m_table->hdr) + m_table->hdr->length;
      return SubtableView{start, end};
    });
  }
};
} // namespace kernel::drivers::acpi
