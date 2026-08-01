#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strcpy.cpp"

class StringCopyTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 1024;
	static constexpr char kSentinel = static_cast<char>(0xAA);

	std::vector<char> src_buffer;
	std::vector<char> dest_buffer;
	std::vector<char> expected_buffer;

	void SetUp() override {
		src_buffer.resize(kBufferSize, kSentinel);
		dest_buffer.resize(kBufferSize, kSentinel);
		expected_buffer.resize(kBufferSize, kSentinel);
	}

	void ResetDest() {
		std::fill(dest_buffer.begin(), dest_buffer.end(), kSentinel);
		std::fill(expected_buffer.begin(), expected_buffer.end(), kSentinel);
	}
};

TEST_F(StringCopyTest, StrncpyPadsWithZeros) {
	const char *payload = "Kernel";
	std::strcpy(src_buffer.data(), payload);

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;

	klibc::strncpy(dest, src_buffer.data(), 16);
	std::strncpy(expected, src_buffer.data(), 16);

	EXPECT_EQ(dest_buffer, expected_buffer);
}

TEST_F(StringCopyTest, StrncpyTruncatesWithoutNull) {
	const char *payload = "System Initialization";
	std::strcpy(src_buffer.data(), payload);

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;

	klibc::strncpy(dest, src_buffer.data(), 6);
	std::strncpy(expected, src_buffer.data(), 6);

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_EQ(dest[6], kSentinel);
}

TEST_F(StringCopyTest, StrncpyZeroCountDoesNothing) {
	char *dest = dest_buffer.data() + 100;

	char *ret = klibc::strncpy(dest, "Kernel", 0);

	EXPECT_EQ(ret, dest);
	EXPECT_EQ(dest_buffer, expected_buffer);
}

TEST_F(StringCopyTest, StrcpyCopiesNullTerminator) {
	const char *payload = "Erebus OS";
	std::strcpy(src_buffer.data(), payload);

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;

	klibc::strcpy(dest, src_buffer.data());
	std::strcpy(expected, src_buffer.data());

	EXPECT_EQ(dest_buffer, expected_buffer);

	EXPECT_EQ(dest[9], '\0');
	EXPECT_EQ(dest[10], kSentinel);
}

TEST_F(StringCopyTest, StrcpyEmptyString) {
	src_buffer[0] = '\0';

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;

	klibc::strcpy(dest, src_buffer.data());
	std::strcpy(expected, src_buffer.data());

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_EQ(dest[0], '\0');
	EXPECT_EQ(dest[1], kSentinel);
}
