#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strcmp.cpp"

class StringCompareTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 8192;
	static constexpr std::uint8_t kSentinel = 0xAA;

	std::vector<char> buffer1;
	std::vector<char> buffer2;

	void SetUp() override {
		buffer1.resize(kBufferSize, static_cast<char>(kSentinel));
		buffer2.resize(kBufferSize, static_cast<char>(kSentinel));
	}

	void ResetBuffers() {
		std::fill(buffer1.begin(), buffer1.end(), static_cast<char>(kSentinel));
		std::fill(buffer2.begin(), buffer2.end(), static_cast<char>(kSentinel));
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

TEST_F(StringCompareTest, StrcmpDelegatesCorrectly) {
	char *s1 = buffer1.data() + 100;
	char *s2 = buffer2.data() + 100;

	std::strcpy(s1, "Erebus OS Kernel");
	std::strcpy(s2, "Erebus OS Kernel");
	EXPECT_EQ(klib::strcmp(s1, s2), 0);

	std::strcpy(s2, "Erebus OS");
	AssertSignMatch(klib::strcmp(s1, s2), std::strcmp(s1, s2), "s1 > s2");

	std::strcpy(s1, "Erebus");
	AssertSignMatch(klib::strcmp(s1, s2), std::strcmp(s1, s2), "s1 < s2");
}

TEST_F(StringCompareTest, ZeroCountReturnsZero) {
	char *s1 = buffer1.data() + 100;
	char *s2 = buffer2.data() + 100;
	std::strcpy(s1, "Apple");
	std::strcpy(s2, "Banana");

	EXPECT_EQ(klib::strncmp(s1, s2, 0), 0);
}

TEST_F(StringCompareTest, DifferenceAtNullByte) {
	char *s1 = buffer1.data() + 128;
	char *s2 = buffer2.data() + 128;

	std::strcpy(s1, "Kernel");
	std::strcpy(s2, "KernelPanic");

	int actual = klib::strncmp(s1, s2, 64);
	int expected = std::strncmp(s1, s2, 64);

	AssertSignMatch(actual, expected, "Mismatch at Null Terminator");
}

TEST_F(StringCompareTest, MismatchAfterNullByteIsIgnored) {
	char *s1 = buffer1.data() + 128;
	char *s2 = buffer2.data() + 128;

	std::memcpy(s1, "Cat\0ZZZZ", 8);
	std::memcpy(s2, "Cat\0AAAA", 8);

	int actual = klib::strncmp(s1, s2, 8);
	EXPECT_EQ(actual, 0) << "Failed to stop SWAR comparison at null byte!";
}

TEST_F(StringCompareTest, PageBoundarySafeGuards) {
	const char *target = "System Initialization Complete";
	const std::size_t len = std::strlen(target);

	const std::size_t page_boundary = 4096;

	const std::vector<int> danger_offsets = {-1, -2, -3, -4, -5, -6, -7, -8};

	for (const int offset: danger_offsets) {
		ResetBuffers();

		char *s1 = buffer1.data() + page_boundary + offset;
		char *s2 = buffer2.data() + page_boundary + offset;

		std::strcpy(s1, target);
		std::strcpy(s2, target);

		EXPECT_EQ(klib::strncmp(s1, s2, len + 1), 0) << "Exact match failed at page offset " << offset;

		s2[len - 2] = 'X';

		int actual = klib::strncmp(s1, s2, len + 1);
		int expected = std::strncmp(s1, s2, len + 1);

		AssertSignMatch(actual, expected, "Mismatch comparison failed at page edge");
	}
}
