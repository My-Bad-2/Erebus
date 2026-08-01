#include "../include/string.hpp"
#include "string.h"

namespace klibc {
	char *strchr(const char *str, const int ch) noexcept {
		using namespace string;

		const auto base = reinterpret_cast<std::uintptr_t>(str);
		const std::uint64_t v8 = broadcast_byte(static_cast<std::uint8_t>(ch));

		const std::uint32_t offset = base & 7UL;
		std::uintptr_t current = base - offset;

		const auto w = unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(current));

		const auto m_c = has_zero_byte<std::uint64_t>(w ^ v8) >> (offset * 8);
		const auto m_0 = has_zero_byte<std::uint64_t>(w) >> (offset * 8);

		if (const std::uint64_t mask = m_c | m_0; mask != 0) {
			const int tz_c = std::countr_zero(m_c);
			const int tz_0 = std::countr_zero(m_0);

			if (tz_c <= tz_0) {
				return const_cast<char *>(str) + (tz_c / 8);
			}

			return nullptr;
		}

		current += 8;

		struct ChunkResult {
			bool stop;
			char *ptr;
		};

		auto check_chunk = [v8](const std::uintptr_t curr) -> ChunkResult {
			const auto w_chunk = unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(curr));
			const auto mc = has_zero_byte<std::uint64_t>(w_chunk ^ v8);
			const auto m0 = has_zero_byte<std::uint64_t>(w_chunk);

			if (const std::uint64_t mask = mc | m0; mask != 0) {
				const int tz_c = std::countr_zero(mc);
				const int tz_0 = std::countr_zero(m0);

				if (tz_c <= tz_0) {
					return {.stop = true, .ptr = reinterpret_cast<char *>(curr) + (tz_c / 8)};
				}

				return {.stop = true, .ptr = nullptr};
			}

			return {.stop = false, .ptr = nullptr};
		};

		while (true) {
			if (auto [stop, ptr] = check_chunk(current); stop) {
				return ptr;
			}

			if (auto [stop, ptr] = check_chunk(current + 8); stop) {
				return ptr;
			}

			if (auto [stop, ptr] = check_chunk(current + 16); stop) {
				return ptr;
			}

			if (auto [stop, ptr] = check_chunk(current + 24); stop) {
				return ptr;
			}

			current += 32;
		}
	}

	char *strrchr(const char *str, const int ch) noexcept {
		using namespace string;
		std::size_t len = strlen(str);

		if (static_cast<char>(ch) == '\0') {
			return const_cast<char *>(str + len);
		}

		while (len > 0 && (reinterpret_cast<std::uintptr_t>(str + len) & 7UL) != 0) {
			--len;

			if (str[len] == static_cast<char>(ch)) {
				return const_cast<char *>(str + len);
			}
		}

		auto curr = reinterpret_cast<std::uintptr_t>(str + len);
		const auto base = reinterpret_cast<std::uintptr_t>(str);
		const std::uint64_t v8 = broadcast_byte(static_cast<std::uint8_t>(ch));

		while (curr >= base + 8) {
			curr -= 8;
			const auto w = unaligned_load<std::uint64_t>(reinterpret_cast<const void *>(curr));

			if (const auto m = has_zero_byte<std::uint64_t>(w ^ v8); m != 0) {
				const std::uint32_t lz = std::countl_zero(m);
				const std::uint32_t byte_idx = (63 - lz) / 8;
				return reinterpret_cast<char *>(curr) + byte_idx;
			}
		}

		while (curr > base) {
			curr -= 1;

			if (*reinterpret_cast<const char *>(curr) == static_cast<char>(ch)) {
				return reinterpret_cast<char *>(curr);
			}
		}

		return nullptr;
	}
} // namespace klibc
