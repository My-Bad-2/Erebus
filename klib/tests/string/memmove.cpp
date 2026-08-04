#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "../../src/string/memmove.cpp"

class MemmoveTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;

	std::vector<std::uint8_t> actual_buffer;
	std::vector<std::uint8_t> expected_buffer;

	void SetUp() override {
		actual_buffer.resize(kBufferSize);
		expected_buffer.resize(kBufferSize);
		ResetBuffers();
	}

	void ResetBuffers() {
		std::iota(actual_buffer.begin(), actual_buffer.end(), 0);
		std::iota(expected_buffer.begin(), expected_buffer.end(), 0);
	}
};

TEST_F(MemmoveTest, ZeroCountDoesNothing) {
	void *dest = actual_buffer.data() + 10;
	const void *src = actual_buffer.data() + 20;

	void *ret = klib::memmove(dest, src, 0);

	EXPECT_EQ(ret, dest);
	EXPECT_EQ(actual_buffer, expected_buffer);
}

TEST_F(MemmoveTest, SamePointerDoesNothing) {
	void *dest = actual_buffer.data() + 100;

	void *ret = klib::memmove(dest, dest, 64);

	EXPECT_EQ(ret, dest);
	EXPECT_EQ(actual_buffer, expected_buffer);
}

TEST_F(MemmoveTest, ExhaustiveOverlapAndAlignments) {
	const std::vector<std::size_t> sizes = {
			1,	2,	 3,		4,	 5,	 7, // <= 8 chunk
			8,	9,	 15, // 8 to 16 chunk
			16, 17,	 31, // 16 to 32 chunk
			32, 33,	 63, // 32 to 64 chunk
			64, // Exactly 64
			65, 127, 128, 192, 256 // > 64 bytes
	};

	const std::size_t base_src_offset = 2048;

	for (const std::size_t count: sizes) {
		const std::vector<int> overlap_shifts = {
				-static_cast<int>(count + 16), // Dest way before Src (No Overlap)
				-static_cast<int>(count / 2), // Dest slightly before Src (Forward Overlap)
				-1, // Dest 1 byte before Src (Forward Overlap)
				0, // Dest == Src (Exact Overlap)
				1, // Dest 1 byte after Src (Backward Overlap)
				static_cast<int>(count / 2), // Dest slightly after Src (Backward Overlap)
				static_cast<int>(count + 16) // Dest way after Src (No Overlap)
		};

		for (const int shift: overlap_shifts) {
			for (const std::size_t src_align: {0, 1, 3, 7}) {
				for (const std::size_t dest_align: {0, 1, 3, 7}) {
					ResetBuffers();

					const std::size_t src_idx = base_src_offset + src_align;
					const std::size_t dest_idx = static_cast<std::size_t>(static_cast<int>(base_src_offset) + shift) + dest_align;

					void *actual_dest = actual_buffer.data() + dest_idx;
					const void *actual_src = actual_buffer.data() + src_idx;

					void *expected_dest = expected_buffer.data() + dest_idx;
					const void *expected_src = expected_buffer.data() + src_idx;

					klib::memmove(actual_dest, actual_src, count);
					std::memmove(expected_dest, expected_src, count);

					ASSERT_EQ(actual_buffer, expected_buffer) << "Mismatch at Size: " << count << " | Shift: " << shift
																										<< " | Src Align: " << src_align << " | Dest Align: " << dest_align;
				}
			}
		}
	}
}
