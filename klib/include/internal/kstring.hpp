#pragma once

#include <cstdint>
#include <string_view>

namespace klib::internal {
	template<std::size_t N>
	struct kstring {
		char chars[N]{};

		consteval kstring(const char (&str)[N]) {
			for (std::size_t i = 0; i < N; ++i) {
				chars[i] = str[i];
			}
		}

		[[nodiscard]] constexpr std::string_view view() const noexcept { return {chars, N - 1}; }

		template<std::size_t M>
		consteval bool operator==(const kstring<M> &other) const noexcept {
			return this->view() == other.view();
		}
	};

	template<std::size_t N>
	kstring(const char (&)[N]) -> kstring<N>;
} // namespace klib::internal
