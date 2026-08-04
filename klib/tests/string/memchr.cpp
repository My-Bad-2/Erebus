#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "../../src/string/memchr.cpp"

class MemchrTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	std::vector<std::uint8_t> buffer;

	void SetUp() override {
		buffer.resize(kBufferSize);
		ResetBuffer();
	}

	void ResetBuffer() {
		for (std::size_t i = 0; i < kBufferSize; ++i) {
			buffer[i] = static_cast<std::uint8_t>(i % 254);
		}
	}
};

TEST_F(MemchrTest, ZeroCountReturnsNullptr) {
	const void *ptr = buffer.data() + 128;
	EXPECT_EQ(klib::memchr(ptr, 0xFF, 0), nullptr);
}

TEST_F(MemchrTest, TargetNotFoundReturnsNullptr) {
	const void *ptr = buffer.data() + 128;
	EXPECT_EQ(klib::memchr(ptr, 0xFF, 256), nullptr);
}

TEST_F(MemchrTest, TreatsCharAsUnsigned) {
	std::uint8_t *ptr = buffer.data() + 128;
	ptr[10] = 0xFF; // 255 in unsigned space

	void *expected = std::memchr(ptr, -1, 32);
	void *actual = klib::memchr(ptr, -1, 32);

	ASSERT_EQ(actual, expected) << "Failed to cast `ch` to unsigned char!";
	ASSERT_EQ(actual, ptr + 10);
}

TEST_F(MemchrTest, FirstOccurrenceGuarantee) {
	const std::size_t count = 35;
	std::uint8_t *ptr = buffer.data() + 512;

	ptr[33] = 0xFF;
	ptr[34] = 0xFF;

	void *expected = std::memchr(ptr, 0xFF, count);
	void *actual = klib::memchr(ptr, 0xFF, count);

	ASSERT_EQ(actual, expected) << "Failed to respect first-occurrence in overlapping tail!";
	ASSERT_EQ(actual, ptr + 33);
}

TEST_F(MemchrTest, ExhaustiveSizesAndAlignments) {
	const std::vector<std::size_t> sizes = {
			1,	2,	 3,		4,	 5,	 7, // <= 8 chunk
			8,	9,	 15, // 8 to 16 chunk
			16, 17,	 31, // 16 to 32 chunk
			32, 33,	 63, // 32 to 64 chunk
			64, // Exactly 64
			65, 127, 128, 192, 256 // > 64 bytes (Unrolled loop + tail)
	};

	const std::vector<std::size_t> alignments = {0, 1, 3, 7};
	const std::size_t anchor = 512;

	for (const std::size_t count: sizes) {
		const std::vector<std::size_t> target_indices = {0, count / 2, count - 1};

		for (const std::size_t align: alignments) {
			for (const std::size_t target_idx: target_indices) {
				ResetBuffer();

				std::uint8_t *ptr = buffer.data() + anchor + align;
				ptr[target_idx] = 0xFF;

				void *expected = std::memchr(ptr, 0xFF, count);
				void *actual = klib::memchr(ptr, 0xFF, count);

				ASSERT_EQ(actual, expected) << "Mismatch at Size: " << count << " | Target Index: " << target_idx
																		<< " | Alignment: " << align;
			}
		}
	}
}
