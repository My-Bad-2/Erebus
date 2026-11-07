/// @file table.hpp
/// @brief Safe access to ACPI tables and their variable-length subtables.
///
/// Firmware tables are input like any other: lengths can be wrong and entries truncated. The
/// types here never hand out a pointer that runs past the end of its table. Consumers still
/// have to check that an entry is long enough for the struct they read it as, which
/// entry_as() does.

#pragma once

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <utility>

#include "parser.hpp"

namespace kernel::drivers::acpi {
/// @brief The subtables that follow a table's fixed part (MADT, SRAT, ...).
///
/// Iteration stops at the first entry whose header does not fit, whose length is shorter than
/// a header, or that would run past the end of the table, so a bad entry hides the rest of the
/// table rather than sending the reader into memory that is not part of it.
class SubtableView {
  const std::uint8_t* m_begin{nullptr};
  const std::uint8_t* m_end{nullptr};

  // @p p if a whole, well-formed entry starts there, @p end otherwise.
  [[nodiscard]] static const std::uint8_t* validate(const std::uint8_t* p,
                                                    const std::uint8_t* end) noexcept {
    if (end - p < static_cast<std::ptrdiff_t>(sizeof(acpi_entry_hdr))) {
      return end;
    }

    const auto* hdr = reinterpret_cast<const acpi_entry_hdr*>(p);
    if (hdr->length < sizeof(acpi_entry_hdr) || hdr->length > end - p) [[unlikely]] {
      return end;
    }

    return p;
  }

 public:
  class Iterator {
    const std::uint8_t* m_current{nullptr};
    const std::uint8_t* m_end{nullptr};

   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const acpi_entry_hdr*;
    using difference_type = std::ptrdiff_t;

    Iterator() noexcept = default;
    Iterator(const std::uint8_t* curr, const std::uint8_t* end) noexcept
        : m_current(curr), m_end(end) {}

    value_type operator*() const noexcept { return reinterpret_cast<value_type>(m_current); }

    Iterator& operator++() noexcept {
      m_current = validate(m_current + (**this)->length, m_end);
      return *this;
    }

    Iterator operator++(int) noexcept {
      const Iterator tmp = *this;
      ++*this;
      return tmp;
    }

    bool operator==(const Iterator& other) const noexcept { return m_current == other.m_current; }
  };

  SubtableView(const std::uint8_t* begin, const std::uint8_t* end) noexcept
      : m_begin(begin), m_end(end) {}

  [[nodiscard]] Iterator begin() const noexcept { return {validate(m_begin, m_end), m_end}; }
  [[nodiscard]] Iterator end() const noexcept { return {m_end, m_end}; }
};

/// @brief @p hdr read as subtable type @p T, or null if the entry is too short to be one.
template <AcpiStruct T>
[[nodiscard]] const T* entry_as(const acpi_entry_hdr* hdr) noexcept {
  return hdr->length >= sizeof(T) ? reinterpret_cast<const T*>(hdr) : nullptr;
}

/// @brief A reference to one installed ACPI table, released again when it goes away.
///
/// Pointers into the table (from as(), payload() or the header) are only good for as long as
/// the Table they came from.
class Table {
  // Held by value: uACPI fills in a handle for us, and pointing at that would leave us holding the
  // address of find()'s stack frame.
  uacpi_table m_table{};

  explicit Table(const uacpi_table& tbl) noexcept : m_table(tbl) {}

  void release() noexcept {
    if (m_table.hdr) {
      uacpi_table_unref(&m_table);
      m_table = {};
    }
  }

 public:
  Table() = default;
  ~Table() noexcept { release(); }

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  Table(Table&& other) noexcept : m_table(std::exchange(other.m_table, uacpi_table{})) {}

  Table& operator=(Table&& other) noexcept {
    if (this != &other) {
      release();
      m_table = std::exchange(other.m_table, uacpi_table{});
    }

    return *this;
  }

  template <Signature Sig>
  [[nodiscard]] static std::expected<Table, uacpi_status> find() noexcept {
    uacpi_table out_table{};
    const Status st = uacpi_table_find_by_signature(Sig.data, &out_table);

    if (st != UACPI_STATUS_OK) [[unlikely]] {
      return std::unexpected(st);
    }

    return Table{out_table};
  }

  [[nodiscard]] const acpi_sdt_hdr* header() const noexcept { return m_table.hdr; }
  [[nodiscard]] std::uint32_t length() const noexcept {
    return m_table.hdr ? m_table.hdr->length : 0;
  }
  [[nodiscard]] std::uint8_t revision() const noexcept {
    return m_table.hdr ? m_table.hdr->revision : 0;
  }

  /// @brief The table as @p T.
  /// @param min_length How long the table must be. Defaults to all of @p T; pass less for
  ///                   tables whose later revisions grew, and check length() before reading
  ///                   the newer fields.
  template <AcpiStruct T>
  [[nodiscard]] std::expected<const T*, uacpi_status> as(
      const std::size_t min_length = sizeof(T)) const noexcept {
    if (!m_table.hdr) [[unlikely]] {
      return std::unexpected(UACPI_STATUS_NOT_FOUND);
    }

    if (m_table.hdr->length < min_length || m_table.hdr->length < sizeof(acpi_sdt_hdr))
        [[unlikely]] {
      return std::unexpected(UACPI_STATUS_INVALID_ARGUMENT);
    }

    return reinterpret_cast<const T*>(m_table.hdr);
  }

  /// @brief The subtables after the fixed part @p T.
  template <AcpiStruct T>
  [[nodiscard]] std::expected<SubtableView, uacpi_status> payload() const noexcept {
    return as<T>().transform([this](const T* base_struct) {
      const auto* start = reinterpret_cast<const std::uint8_t*>(base_struct) + sizeof(T);
      const auto* end = reinterpret_cast<const std::uint8_t*>(m_table.hdr) + m_table.hdr->length;
      return SubtableView{start, end};
    });
  }
};
}  // namespace kernel::drivers::acpi
