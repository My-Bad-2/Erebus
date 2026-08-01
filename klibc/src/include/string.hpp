#ifndef EREBUS_KLIBC_SRC_STRING_HELPERS_HPP
#define EREBUS_KLIBC_SRC_STRING_HELPERS_HPP

#include <bit>
#include <cstdint>
#include <type_traits>

namespace klibc::string {
	template<typename T>
	struct [[gnu::packed, gnu::may_alias]] unaligned_t {
		T val;
	};

	/**
	 * Safely stores an unaligned integer in memory
	 */
	template<typename T>
		requires std::is_integral_v<T> && std::is_trivially_copyable_v<T>
	[[gnu::always_inline]] void unaligned_store(void *ptr, T val) noexcept {
		static_cast<unaligned_t<T> *>(ptr)->val = val;
	}

	/**
	 * Safely loads an unaligned integer from memory
	 */
	template<typename T>
		requires std::is_integral_v<T> && std::is_trivially_copyable_v<T>
	[[gnu::always_inline]] T unaligned_load(const void *ptr) noexcept {
		return static_cast<const unaligned_t<T> *>(ptr)->val;
	}

	/**
	 * @brief Evaluates a n-byte word to see if any of its bytes are 0x00.
	 * @return Non-zero if at least one byte is 0x00, zero otherwise.
	 */
	template<typename T>
	[[nodiscard, gnu::const, gnu::always_inline]]
	constexpr T has_zero_byte(const T v) noexcept {
		// Subtracting 0x01 from 0x00 underflows the byte to 0xFF, setting the MSB.
		// We then mask against the inverted original word and 0x80 to isolate the MSBs.
		if constexpr (sizeof(T) == 8) {
			return (v - 0x0101010101010101UL) & (~v & 0x8080808080808080UL);
		} else if constexpr (sizeof(T) == 4) {
			return (v - 0x01010101U) & (~v & 0x80808080U);
		} else if constexpr (sizeof(T) == 2) {
			return (v - 0x0101U) & (~v & 0x8080U);
		} else {
			return static_cast<T>((v - 0x01U) & (~v & 0x80U));
		}
	}

	/**
	 * @brief Broadcasts an 8-bit value across a 64-bit word.
	 */
	[[nodiscard, gnu::const, gnu::always_inline]] constexpr std::uint64_t broadcast_byte(const std::uint8_t c) noexcept {
		if constexpr (__builtin_constant_p(c)) {
			return static_cast<std::uint64_t>(c) * 0x0101010101010101UL;
		}

		if (c == 0) [[likely]] {
			return 0;
		}

		// Multiplies the single byte across all 8 byte lanes.
		return static_cast<std::uint64_t>(c) * 0x0101010101010101UL;
	}

	/**
	 * @brief Evaluates an 8-byte word to see if it contains a specific byte.
	 */
	[[nodiscard, gnu::const, gnu::always_inline]]
	constexpr std::uint64_t has_byte(const std::uint64_t v, const std::uint8_t target) noexcept {
		// XORing the memory word with the broadcasted target byte turns all
		// matching bytes into 0x00. Then we just look for a zero byte.
		return has_zero_byte<std::uint64_t>(v ^ broadcast_byte(target));
	}

	[[gnu::always_inline]]
	inline void hw_rep_movsb(void *dest, const void *src, std::size_t count) noexcept {
		void *d = dest;
		void *s = const_cast<void *>(src);
		auto c = count;

		asm volatile("rep movsb"
								 : "+D"(d), "+S"(s), "+c"(c) // Outputs: RDI, RSI, RCX
								 :
								 : "memory");
	}

	[[gnu::always_inline]]
	inline void hw_rep_stosb(void *dest, std::uint8_t val, std::size_t count) noexcept {
		void *d = dest;
		auto c = count;

		asm volatile("rep stosb"
								 : "+D"(d), "+c"(c) // Outputs: RDI and RCX advanced
								 : "a"(val) // Inputs: AL contains the byte
								 : "memory" // Clobber: memory modified
		);
	}

	/**
	 * @brief Copy a chunk of memory.
	 */
	template<typename T>
	[[gnu::always_inline]]
	void copy_chunk(const std::uintptr_t d_base, const std::uintptr_t s_base, const std::size_t offset) noexcept {
		// NOLINTNEXTLINE(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
		const auto val = unaligned_load<T>(reinterpret_cast<const void *>(s_base + offset));

		// NOLINTNEXTLINE(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
		unaligned_store<T>(reinterpret_cast<void *>(d_base + offset), val);
	}

	/**
	 * Calculates the exact difference between the first mismatching bytes in two little-endian integers.
	 */
	template<typename T>
		requires std::is_integral_v<T>
	[[gnu::always_inline]] int cmp_diff(T v1, T v2) noexcept {
		const auto diff = static_cast<std::uint64_t>(v1 ^ v2);

		// CTZ locates the lowest differing bit. `~7U` snaps it to the exact byte boundary.
		const std::uint32_t shift = static_cast<std::uint32_t>(std::countr_zero(diff)) & ~7U;

		const int byte1 = static_cast<int>((static_cast<std::uint64_t>(v1) >> shift) & 0xFFU);
		const int byte2 = static_cast<int>((static_cast<std::uint64_t>(v2) >> shift) & 0xFFU);

		return byte1 - byte2;
	}

	template<typename T>
	[[gnu::always_inline]]
	int check_chunk(const std::uintptr_t p1, const std::uintptr_t p2, const std::size_t off) noexcept {
		const auto v1 = unaligned_load<T>(reinterpret_cast<const void *>(p1 + off));
		const auto v2 = unaligned_load<T>(reinterpret_cast<const void *>(p2 + off));

		if (v1 != v2) {
			if constexpr (sizeof(T) == 1) {
				return static_cast<int>(v1) - static_cast<int>(v2);
			} else {
				return cmp_diff(v1, v2);
			}
		}

		return 0;
	}

	template<typename T>
	[[gnu::always_inline]]
	void *check_chunk_offset(std::uintptr_t base, std::size_t off, std::uint64_t broadcast) noexcept {
		const auto v = unaligned_load<T>(reinterpret_cast<const void *>(base + off));

		if constexpr (sizeof(T) == 1) {
			if (v == static_cast<std::uint8_t>(broadcast)) {
				return reinterpret_cast<void *>(base + off);
			}
		} else {
			const T b = static_cast<T>(broadcast);

			if (const T mask = has_zero_byte<T>(v ^ b)) {
				const std::uint32_t shift = std::countr_zero(mask);
				return reinterpret_cast<void *>(base + off + (shift / 8));
			}
		}

		return nullptr;
	}

	template<typename T, typename... Offsets>
	[[gnu::always_inline]]
	void transfer(std::uintptr_t d_base, std::uintptr_t s_base, Offsets... offsets) noexcept {
		// Compiler will unroll this completely, loading the requested memory directly into CPU GPRs.
		const T snapshot[] = {
				unaligned_load<T>(reinterpret_cast<const void *>(s_base + static_cast<std::size_t>(offsets)))...};

		std::size_t index = 0;
		(unaligned_store<T>(reinterpret_cast<void *>(d_base + static_cast<std::size_t>(offsets)), snapshot[index++]), ...);
	}
} // namespace klibc::string

#endif // EREBUS_KLIBC_SRC_STRING_HELPERS_HPP
