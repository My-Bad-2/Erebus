#ifndef EREBUS_KLIB_INCLUDE_BITFIELD_HPP
#define EREBUS_KLIB_INCLUDE_BITFIELD_HPP

#include <algorithm>

#include "internal/kstring.hpp"
#include "kformat/formatter.hpp"

namespace klib {
	enum class Access : std::uint8_t { RW, RO, WO };

	template<internal::kstring Name, std::size_t Offset, std::size_t Width = 1,
					 Access acc = Access::RW>
	struct Field {
		static constexpr internal::kstring name = Name;
		static constexpr std::size_t offset = Offset;
		static constexpr std::size_t width = Width;
		static constexpr std::size_t max_bit = Offset + Width - 1;
		static constexpr Access access = acc;
		static constexpr bool is_reserved = false;

		static_assert(Width > 0, "Field width must be greater than 0");
	};

	template<internal::kstring Name, std::size_t Offset, std::size_t Width = 1>
	using ReadOnly = Field<Name, Offset, Width, Access::RO>;

	template<internal::kstring Name, std::size_t Offset, std::size_t Width = 1>
	using WriteOnly = Field<Name, Offset, Width, Access::WO>;

	template<internal::kstring Name, std::size_t Offset>
	using Bit = Field<Name, Offset, 1>;

	template<std::size_t Offset, std::size_t Width = 1>
	struct Reserved {
		static constexpr auto name = internal::kstring<1>{""};

		static constexpr std::size_t offset = Offset;
		static constexpr std::size_t width = Width;
		static constexpr std::size_t max_bit = Offset + Width - 1;
		static constexpr bool is_reserved = true;

		static constexpr auto access = Access::RO;
	};

	namespace detail {
		template<internal::kstring Target, typename F, typename... Rest>
		consteval auto find_field() {
			if constexpr (F::is_reserved) {
				if constexpr (sizeof...(Rest) > 0) {
					return find_field<Target, Rest...>();
				} else {
					static_assert(false, "Field name not found in the Schema!");
				}
			}

			if constexpr (F::name == Target) {
				return F{};
			} else if constexpr (sizeof...(Rest) > 0) {
				return find_field<Target, Rest...>();
			} else {
				static_assert(false, "Field name not found in the Schema!");
			}
		}

		template<typename... Fields>
		consteval std::size_t get_max_bit() {
			if constexpr (sizeof...(Fields) == 0) {
				return 0;
			} else {
				std::size_t max_b = 0;

				auto check = [&](std::size_t bit) { max_b = std::max(bit, max_b); };

				(check(Fields::max_bit), ...);
				return max_b;
			}
		}

		template<typename... Fields>
		consteval bool validate_no_overlap() {
			bool used[64] = {false};
			bool valid = true;

			auto check = [&]<typename F>(F) {
				for (std::size_t i = F::offset; i < F::max_bit; ++i) {
					if (used[i]) {
						// overlap detected!
						valid = false;
					}

					used[i] = true;
				}
			};

			(check(Fields{}), ...);
			return valid;
		}

		template<std::size_t MaxBit>
		using hw_type_t = decltype([]<std::size_t M = MaxBit> {
			if constexpr (M < 8) {
				return std::uint8_t{};
			} else if constexpr (M < 16) {
				return std::uint16_t{};
			} else if constexpr (M < 32) {
				return std::uint32_t{};
			} else {
				return std::uint64_t{};
			}
		}());
	} // namespace detail

	template<std::size_t Offset, typename T, Access Acc = Access::RW>
	struct Register {
		static constexpr std::size_t offset = Offset;
		static constexpr Access access = Acc;
		using ValueType = T;
		using HwType = detail::hw_type_t<(sizeof(T) * 8) - 1>;
	};

	template<std::size_t BaseOffset, std::size_t Stride, typename T,
					 Access Acc = Access::RW>
	struct RegisterArray {
		static constexpr std::size_t base_offset = BaseOffset;
		static constexpr std::size_t stride = Stride;
		static constexpr Access access = Acc;
		using ValueType = T;
		using HwType = detail::hw_type_t<(sizeof(T) * 8) - 1>;

		static constexpr std::size_t offset_for(const std::size_t index) noexcept {
			return base_offset + (index * stride);
		}
	};

	template<typename... Fields>
	struct BitfieldSchema {
		static_assert(detail::validate_no_overlap<Fields...>(),
									"BitfieldSchema contains overlapping bits!");

		static constexpr std::size_t MAX_BIT = detail::get_max_bit<Fields...>();
		static_assert(MAX_BIT < 64,
									"Bitfields exceeding 64 bits are not supported.");

		using Type = detail::hw_type_t<MAX_BIT>;
		Type data;

		template<internal::kstring... Names>
		static consteval Type generate_mask() {
			Type combined_mask = 0;

			auto apply_mask = [&]<internal::kstring N>() {
				using F = decltype(detail::find_field<N, Fields...>());
				constexpr Type m = (F::width == sizeof(Type) * 8)
															 ? ~Type{0}
															 : (Type{1} << F::width) - 1;
				combined_mask |= m << F::offset;
			};

			(apply_mask.template operator()<Names>(), ...);
			return combined_mask;
		}

		constexpr explicit BitfieldSchema(Type val = 0) noexcept : data(val) {}

		template<internal::kstring Name, typename Ret = Type>
		[[nodiscard]] constexpr Ret get() const noexcept {
			using F = decltype(detail::find_field<Name, Fields...>());

			static_assert(F::access != Access::WO,
										"Attempted to read a Write-Only bitfield!");

			constexpr Type mask =
					(F::width == sizeof(Type) * 8) ? ~Type{0} : (Type{1} << F::width) - 1;
			return static_cast<Ret>((data >> F::offset) & mask);
		}

		template<internal::kstring Name, typename V>
		[[nodiscard]] constexpr BitfieldSchema set(V value) const noexcept {
			using F = decltype(detail::find_field<Name, Fields...>());

			static_assert(F::access != Access::RO,
										"Attempted to write to a Read-Only bitfield!");

			Type raw_val = static_cast<Type>(value);
			constexpr Type mask =
					(F::width == sizeof(Type) * 8) ? ~Type{0} : (Type{1} << F::width) - 1;
			constexpr Type shifted_mask = mask << F::offset;

			BitfieldSchema copy = *this;
			copy.data =
					(copy.data & ~shifted_mask) | ((raw_val << F::offset) & shifted_mask);
			return copy;
		}

		template<internal::kstring Name, typename V>
		constexpr void set_mut(V value) noexcept {
			*this = this->template set<Name>(value);
		}

		constexpr explicit operator Type() const noexcept { return data; }
	};

	template<typename... Fields>
	struct formatter<BitfieldSchema<Fields...>> {
		template<typename Sink>
		static constexpr void format(Sink &sink,
																 const BitfieldSchema<Fields...> &bf,
																 const FormatSpec &) noexcept {
			sink.push('[');
			bool first = true;

			auto print_field = [&]<typename F>(F) {
				if constexpr (!F::is_reserved) {
					auto val = bf.template get<F::name>();

					if (val > 0) {
						if (!first) {
							sink.append(" | ");
						}

						sink.append(F::name.view());

						if constexpr (F::width > 1) {
							sink.push('=');
							FormatSpec num_spec;
							num_spec.base = 16;
							num_spec.alt_form = true;
							formatter<decltype(val)>::format(sink, val, num_spec);
						}

						first = false;
					}
				}
			};

			(print_field(Fields{}), ...);

			if (first) {
				sink.append("NONE");
			}

			sink.push(']');
		}
	};
} // namespace klib

#endif // EREBUS_KLIB_INCLUDE_BITFIELD_HPP
