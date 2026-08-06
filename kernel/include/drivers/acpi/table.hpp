#ifndef EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_TABLE_HPP
#define EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_TABLE_HPP

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

#include <expected>
#include <iterator>
#include <span>
#include <type_traits>
#include <utility>

#include "parser.hpp"

namespace kernel::drivers::acpi {
	class SubtableView {
		const std::uint8_t *m_begin{nullptr};
		const std::uint8_t *m_end{nullptr};

	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = const acpi_entry_hdr *;
		using difference_type = std::ptrdiff_t;

		SubtableView(const std::uint8_t *begin, const std::uint8_t *end) noexcept :
				m_begin(begin), m_end(end) {}

		class Iterator {
			const std::uint8_t *m_current;
			const std::uint8_t *m_end;

		public:
			using iterator_category = std::forward_iterator_tag;
			using value_type = const acpi_entry_hdr *;
			using difference_type = std::ptrdiff_t;

			Iterator(const std::uint8_t *curr, const std::uint8_t *end) noexcept :
					m_current(curr), m_end(end) {}

			value_type operator*() const noexcept {
				return reinterpret_cast<value_type>(m_current);
			}

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

			bool operator!=(const Iterator &other) const noexcept {
				return m_current < other.m_current;
			}
		};

		[[nodiscard]] Iterator begin() const noexcept {
			return Iterator(m_begin, m_end);
		}

		[[nodiscard]] Iterator end() const noexcept {
			return Iterator(m_end, m_end);
		}
	};

	class Table {
		uacpi_table *m_table{nullptr};

		explicit Table(uacpi_table *tbl) noexcept : m_table(tbl) {}

	public:
		Table() = default;

		Table(const Table &) = delete;
		Table &operator=(const Table &) = delete;

		Table(Table &&other) noexcept :
				m_table(std::exchange(other.m_table, nullptr)) {}

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

		template<Signature Sig>
		[[nodiscard]] static std::expected<Table, uacpi_status> find() noexcept {
			uacpi_table out_table{};
			Status st = uacpi_table_find_by_signature(Sig.data, &out_table);

			if (st != UACPI_STATUS_OK) [[unlikely]] {
				return std::unexpected(st);
			}

			return Table{&out_table};
		}

		template<AcpiStruct T>
		[[nodiscard]] std::expected<const T *, uacpi_status> as() const noexcept {
			if (!m_table || !m_table->hdr) [[unlikely]] {
				return std::unexpected(UACPI_STATUS_NOT_FOUND);
			}

			if (m_table->hdr->length < sizeof(T)) [[unlikely]] {
				return std::unexpected(UACPI_STATUS_INVALID_ARGUMENT);
			}

			return reinterpret_cast<const T *>(m_table->hdr);
		}

		template<AcpiStruct T>
		[[nodiscard]] std::expected<SubtableView, uacpi_status>
		payload() const noexcept {
			return as<T>().transform([this](const T *base_struct) {
				const auto *start =
						reinterpret_cast<const std::uint8_t *>(base_struct) + sizeof(T);
				const auto *end = reinterpret_cast<const std::uint8_t *>(m_table->hdr) +
													m_table->hdr->length;
				return SubtableView{start, end};
			});
		}
	};

} // namespace kernel::drivers::acpi

#endif // EREBUS_KERNEL_INCLUDE_DRIVERS_ACPI_TABLE_HPP
