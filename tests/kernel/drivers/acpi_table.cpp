#include <cstdint>
#include <initializer_list>
#include <utility>

#include "drivers/acpi/table.hpp"
#include "ktest.hpp"

namespace kernel::testing {
namespace {
using drivers::acpi::entry_as;
using drivers::acpi::SubtableView;

// The body of a MADT-like table, built out of (type, length) subtables.
struct Table {
  std::uint8_t bytes[256]{};
  std::size_t size = 0;

  Table(const std::initializer_list<std::pair<std::uint8_t, std::uint8_t>> entries) noexcept {
    for (const auto [type, length] : entries) {
      const std::size_t room = length < 2 ? 2 : length;
      for (std::size_t i = 0; i < room; ++i) {
        bytes[size + i] = 0xEE;
      }
      bytes[size] = type;
      bytes[size + 1] = length;
      size += room;
    }
  }

  [[nodiscard]] SubtableView view(const std::size_t length) const noexcept {
    return {bytes, bytes + length};
  }

  [[nodiscard]] SubtableView view() const noexcept { return view(size); }
};

// Whether walking `view` yields exactly the given subtable types.
[[nodiscard]] bool yields(const SubtableView& view,
                          const std::initializer_list<std::uint8_t> types) noexcept {
  const std::uint8_t* next = types.begin();
  for (const acpi_entry_hdr* hdr : view) {
    if (next == types.end() || hdr->type != *next) {
      return false;
    }
    ++next;
  }

  return next == types.end();
}
}  // namespace

KTEST(SubtableView, walks_well_formed_entries) {
  const Table table{{0, 8}, {1, 12}, {2, 10}};
  KEXPECT(yields(table.view(), {0, 1, 2}));
  KEXPECT(yields(table.view(0), {}));
}

KTEST(SubtableView, stops_at_an_entry_running_past_the_end) {
  // The table claims to end in the middle of the second entry.
  const Table table{{0, 8}, {1, 12}};
  KEXPECT(yields(table.view(14), {0}));
}

// A length below the header size would loop forever or walk backwards.
KTEST(SubtableView, stops_at_a_length_shorter_than_the_header) {
  const Table zero{{0, 8}, {7, 0}, {1, 8}};
  KEXPECT(yields(zero.view(), {0}));

  const Table one{{0, 8}, {7, 1}, {1, 8}};
  KEXPECT(yields(one.view(), {0}));
}

KTEST(SubtableView, stops_at_a_truncated_header) {
  // One byte of the second header is inside the table, its length byte is not.
  const Table table{{0, 8}, {1, 8}};
  KEXPECT(yields(table.view(9), {0}));
}

KTEST(EntryAs, refuses_entries_shorter_than_the_struct) {
  constexpr auto full = static_cast<std::uint8_t>(sizeof(acpi_madt_ioapic));
  const Table table{{ACPI_MADT_ENTRY_TYPE_IOAPIC, full},
                    {ACPI_MADT_ENTRY_TYPE_IOAPIC, static_cast<std::uint8_t>(full - 1)}};
  const auto* first = reinterpret_cast<const acpi_entry_hdr*>(table.bytes);
  const auto* second = reinterpret_cast<const acpi_entry_hdr*>(table.bytes + full);

  KEXPECT_NE(entry_as<acpi_madt_ioapic>(first), nullptr);
  KEXPECT_EQ(entry_as<acpi_madt_ioapic>(second), nullptr);
}
}  // namespace kernel::testing
