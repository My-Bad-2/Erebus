#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "../../src/string/memcpy.cpp"

class MemcpyTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	static constexpr std::uint8_t kSentinel = 0xAA;

	std::vector<std::uint8_t> src_buffer;
	std::vector<std::uint8_t> actual_dest_buffer;
	std::vector<std::uint8_t> expected_dest_buffer;

	void SetUp() override {
		src_buffer.resize(kBufferSize);
		actual_dest_buffer.resize(kBufferSize, kSentinel);
		expected_dest_buffer.resize(kBufferSize, kSentinel);

		std::iota(src_buffer.begin(), src_buffer.end(), 0);
	}

	void ResetDestBuffers() {
		std::fill(actual_dest_buffer.begin(), actual_dest_buffer.end(), kSentinel);
		std::fill(expected_dest_buffer.begin(), expected_dest_buffer.end(), kSentinel);
	}

	void VerifyBounds(std::size_t dest_offset, std::size_t count) {
		for (std::size_t i = 0; i < dest_offset; ++i) {
			ASSERT_EQ(actual_dest_buffer[i], kSentinel) << "Destination Buffer UNDERRUN detected at index " << i;
		}

		for (std::size_t i = dest_offset + count; i < kBufferSize; ++i) {
			ASSERT_EQ(actual_dest_buffer[i], kSentinel) << "Destination Buffer OVERRUN detected at index " << i;
		}
	}
};

TEST_F(MemcpyTest, ZeroCountDoesNothing) {
	void *dest = actual_dest_buffer.data() + 10;
	const void *src = src_buffer.data() + 10;

	void *ret = klib::memcpy(dest, src, 0);

	EXPECT_EQ(ret, dest);
	EXPECT_EQ(actual_dest_buffer, expected_dest_buffer);
}

TEST_F(MemcpyTest, ReturnsDestinationPointer) {
	const std::size_t offset = 128;
	void *dest = actual_dest_buffer.data() + offset;
	const void *src = src_buffer.data() + offset;

	void *ret = klib::memcpy(dest, src, 64);
	EXPECT_EQ(ret, dest);
}

TEST_F(MemcpyTest, ExhaustiveSizesAndDualAlignments) {
	const std::vector<std::size_t> sizes = {
			1,	2,	 3,		4,	 5,		7, // <= 8 chunk
			8,	9,	 15, // 8 to 16 chunk
			16, 17,	 31, // 16 to 32 chunk
			32, 33,	 63, // 32 to 64 chunk
			64, // Exactly 64
			65, 127, 128, 512, 1024 // > 64 bytes
	};

	const std::vector<std::size_t> alignments = {0, 1, 2, 3, 4, 5, 6, 7};

	for (const std::size_t count: sizes) {
		for (const std::size_t dest_align: alignments) {
			for (const std::size_t src_align: alignments) {
				ResetDestBuffers();

				void *actual_dest = actual_dest_buffer.data() + dest_align;
				void *expected_dest = expected_dest_buffer.data() + dest_align;
				const void *src = src_buffer.data() + src_align;

				klib::memcpy(actual_dest, src, count);
				std::memcpy(expected_dest, src, count);

				ASSERT_EQ(actual_dest_buffer, expected_dest_buffer)
						<< "Mismatch at Size: " << count << " | Dest Align: " << dest_align << " | Src Align: " << src_align;

				VerifyBounds(dest_align, count);
			}
		}
	}
}
