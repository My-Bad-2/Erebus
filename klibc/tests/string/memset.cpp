#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/memset.cpp"

namespace {
	class MemsetTest : public ::testing::Test {
	protected:
		static constexpr std::size_t kBufferSize = 4096;

		static constexpr std::uint8_t kSentinel = 0xAA;

		std::vector<std::uint8_t> actual_buffer;
		std::vector<std::uint8_t> expected_buffer;

		virtual void SetUp() {
			actual_buffer.resize(kBufferSize, kSentinel);
			expected_buffer.resize(kBufferSize, kSentinel);
		}

		void ResetBuffers() {
			std::ranges::fill(actual_buffer, kSentinel);
			std::ranges::fill(expected_buffer, kSentinel);
		}

		void VerifyBounds(const std::size_t offset, const std::size_t count) const {
			for (std::size_t i = 0; i < offset; ++i) {
				ASSERT_EQ(actual_buffer[i], kSentinel) << "Buffer UNDERRUN detected at index " << i;
			}

			for (std::size_t i = offset + count; i < kBufferSize; ++i) {
				ASSERT_EQ(actual_buffer[i], kSentinel) << "Buffer OVERRUN detected at index " << i;
			}
		}
	};
} // namespace

TEST_F(MemsetTest, ZeroCountDoesNothing) {
	void *dest = actual_buffer.data() + 10;

	void *ret = klibc::memset(dest, 0x00, 0);

	EXPECT_EQ(ret, dest);
	EXPECT_EQ(actual_buffer, expected_buffer);
}

TEST_F(MemsetTest, ReturnsDestinationPointer) {
	const std::size_t offset = 128;
	void *dest = actual_buffer.data() + offset;

	void *ret = klibc::memset(dest, 0xFF, 64);
	EXPECT_EQ(ret, dest);
}

TEST_F(MemsetTest, ExhaustiveSizesAlignmentsAndValues) {
	const std::vector<std::size_t> sizes = {
			1,	2,	 3,		4,	 5,		 7, // 1 to 7 bytes
			8,	9,	 15, // 8 to 15 bytes
			16, 17,	 31, // 16 to 31 bytes
			32, 33,	 63, // 32 to 63 bytes
			64, // Exactly 64
			65, 127, 128, 512, 1024, // >64 bytes
	};

	const std::vector<int> fill_values = {0x00, 0xFF, 0x55, -1, 256};

	const std::vector<std::size_t> alignments = {0, 1, 2, 3, 4, 5, 6, 7};

	for (const std::size_t count: sizes) {
		for (const int ch: fill_values) {
			for (const std::size_t align: alignments) {
				ResetBuffers();

				void *actual_dest = actual_buffer.data() + align;
				void *expected_dest = expected_buffer.data() + align;

				klibc::memset(actual_dest, ch, count);
				std::memset(expected_dest, ch, count);

				ASSERT_EQ(actual_buffer, expected_buffer)
						<< "Mismatch at Size: " << count << " | Value: " << ch << " | Alignment: " << align;

				VerifyBounds(align, count);
			}
		}
	}
}
