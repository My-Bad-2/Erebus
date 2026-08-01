#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "../../src/string/memcmp.cpp"

class MemcmpTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;

	std::vector<std::uint8_t> buffer1;
	std::vector<std::uint8_t> buffer2;

	void SetUp() override {
		buffer1.resize(kBufferSize);
		buffer2.resize(kBufferSize);
		ResetBuffers();
	}

	void ResetBuffers() {
		// Fill buffers with an identical repeating sequence
		std::iota(buffer1.begin(), buffer1.end(), 0);
		std::iota(buffer2.begin(), buffer2.end(), 0);
	}

	void AssertSignMatch(int actual, int expected, const char *context) {
		if (expected == 0) {
			ASSERT_EQ(actual, 0) << "Expected EXACT MATCH (0). " << context;
		} else if (expected > 0) {
			ASSERT_GT(actual, 0) << "Expected POSITIVE (>0). " << context;
		} else {
			ASSERT_LT(actual, 0) << "Expected NEGATIVE (<0). " << context;
		}
	}
};

TEST_F(MemcmpTest, ZeroCountReturnsZero) {
	const void *s1 = buffer1.data() + 10;
	const void *s2 = buffer2.data() + 20;

	EXPECT_EQ(klibc::memcmp(s1, s2, 0), 0);
}

TEST_F(MemcmpTest, IdenticalPointersReturnZero) {
	const void *ptr = buffer1.data() + 128;

	EXPECT_EQ(klibc::memcmp(ptr, ptr, 256), 0);
}

TEST_F(MemcmpTest, LexicographicalEndiannessTrap) {
	std::uint8_t a[] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	std::uint8_t b[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	int kernel_res = klibc::memcmp(a, b, 8);
	int std_res = std::memcmp(a, b, 8);

	AssertSignMatch(kernel_res, std_res, "Endianness Trap: A < B");

	kernel_res = klibc::memcmp(b, a, 8);
	std_res = std::memcmp(b, a, 8);

	AssertSignMatch(kernel_res, std_res, "Endianness Trap: B > A");
}

TEST_F(MemcmpTest, ExhaustiveMatchAndMismatch) {
	const std::vector<std::size_t> sizes = {
			1,	2,	 3,		4,	 5,	 7, // <= 8 chunk
			8,	9,	 15, // 8 to 16 chunk
			16, 17,	 31, // 16 to 32 chunk
			32, 33,	 63, // 32 to 64 chunk
			64, // Exactly 64
			65, 127, 128, 192, 256 // > 64 bytes
	};

	const std::vector<std::size_t> alignments = {0, 1, 3, 7};

	const std::size_t anchor = 512;

	for (const std::size_t count: sizes) {
		const std::vector<std::size_t> diff_indices = {0, count / 2, count - 1};

		for (const std::size_t a1: alignments) {
			for (const std::size_t a2: alignments) {

				const std::uint8_t *p1 = buffer1.data() + anchor + a1;
				const std::uint8_t *p2 = buffer2.data() + anchor + a2;

				ResetBuffers();
				int match_res = klibc::memcmp(p1, p2, count);
				int expected_match = std::memcmp(p1, p2, count);
				AssertSignMatch(match_res, expected_match, "Failed Exact Match");

				for (const std::size_t diff_idx: diff_indices) {
					// Case A: p1 > p2 at diff_idx
					ResetBuffers();
					buffer1[anchor + a1 + diff_idx] = 0xFF; // Max unsigned char
					buffer2[anchor + a2 + diff_idx] = 0x00; // Min unsigned char

					int resA = klibc::memcmp(p1, p2, count);
					int expA = std::memcmp(p1, p2, count);
					AssertSignMatch(resA, expA, "Failed Mismatch A (p1 > p2)");

					// Case B: p1 < p2 at diff_idx
					ResetBuffers();
					buffer1[anchor + a1 + diff_idx] = 0x00;
					buffer2[anchor + a2 + diff_idx] = 0xFF;

					int resB = klibc::memcmp(p1, p2, count);
					int expB = std::memcmp(p1, p2, count);
					AssertSignMatch(resB, expB, "Failed Mismatch B (p1 < p2)");
				}
			}
		}
	}
}
