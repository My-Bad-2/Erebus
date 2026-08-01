#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strlen.cpp"

class StrlenTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;

	static constexpr std::uint8_t kSentinel = 0xAA;

	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, static_cast<char>(kSentinel)); }

	void ResetBuffer() { std::fill(buffer.begin(), buffer.end(), static_cast<char>(kSentinel)); }
};

TEST_F(StrlenTest, EmptyString) {
	char *str = buffer.data() + 100;
	str[0] = '\0';

	EXPECT_EQ(klibc::strlen(str), 0);
}

TEST_F(StrlenTest, PrecedingNullByteTrap) {
	char *aligned_base = buffer.data() + 128;

	const std::size_t offset = 3;
	char *str = aligned_base + offset;

	std::memcpy(str, "Hello", 6);

	aligned_base[1] = '\0';

	EXPECT_EQ(klibc::strlen(str), 5) << "The down-alignment logic failed to ignore preceding null bytes!";
}

TEST_F(StrlenTest, ExhaustiveSizesAndAlignments) {
	const std::vector<std::size_t> lengths = {
			0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 256,
	};

	const std::vector<std::size_t> alignments = {0, 1, 2, 3, 4, 5, 6, 7};

	for (const std::size_t len: lengths) {
		for (const std::size_t align: alignments) {
			const std::size_t anchor = 512;
			ResetBuffer();

			char *str = buffer.data() + anchor + align;

			std::memset(str, 'A', len);

			str[len] = '\0';

			if (len + 1 < kBufferSize - anchor)
				str[len + 1] = '\0';
			if (len + 5 < kBufferSize - anchor)
				str[len + 5] = '\0';

			std::size_t const actual_len = klibc::strlen(str);
			std::size_t const expected_len = std::strlen(str);

			ASSERT_EQ(actual_len, expected_len) << "Mismatch at Length: " << len << " | Alignment Offset: " << align;
		}
	}
}

class StrnlenTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	static constexpr std::uint8_t kSentinel = 0xAA;
	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, static_cast<char>(kSentinel)); }
	void ResetBuffer() { std::fill(buffer.begin(), buffer.end(), static_cast<char>(kSentinel)); }
};

TEST_F(StrnlenTest, ZeroMaxlenReturnsZero) {
	char *str = buffer.data() + 128;
	std::memcpy(str, "Kernel", 7);

	EXPECT_EQ(klibc::strnlen(str, 0), 0);
}

TEST_F(StrnlenTest, PrecedingNullByteTrapWithMaxlen) {
	char *aligned_base = buffer.data() + 128;
	constexpr std::size_t offset = 3;
	char *str = aligned_base + offset;

	std::memcpy(str, "Hello", 6);
	aligned_base[1] = '\0';

	EXPECT_EQ(klibc::strnlen(str, 2), 2);
	EXPECT_EQ(klibc::strnlen(str, 10), 5);
}

TEST_F(StrnlenTest, ExhaustiveSizesAlignmentsAndBounds) {
	const std::vector<std::size_t> lengths = {
			0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 256,
	};
	const std::vector<std::size_t> alignments = {0, 1, 2, 3, 4, 5, 6, 7};

	for (const std::size_t len: lengths) {
		for (const std::size_t align: alignments) {
			constexpr std::size_t anchor = 512;
			ResetBuffer();

			char *str = buffer.data() + anchor + align;
			std::memset(str, 'A', len);
			str[len] = '\0';

			if (len + 1 < kBufferSize - anchor)
				str[len + 1] = '\0';
			if (len + 5 < kBufferSize - anchor)
				str[len + 5] = '\0';

			const std::vector<std::size_t> maxlens = {
					0,
					len > 0 ? len - 1 : 0, // Just under length (truncate)
					len, // Exact length (truncate)
					len + 1, // Just over length (stop at null)
					len + 10, // Way over length (stop at null)
					kBufferSize - anchor - align
			};

			for (const std::size_t maxlen: maxlens) {
				std::size_t const actual_len = klibc::strnlen(str, maxlen);
				std::size_t const expected_len = std::min(len, maxlen);

				ASSERT_EQ(actual_len, expected_len)
						<< "Mismatch at Actual Length: " << len << " | Maxlen: " << maxlen << " | Alignment Offset: " << align;
			}
		}
	}
}
