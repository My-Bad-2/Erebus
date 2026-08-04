#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strcat.cpp"

class StringConcatTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 1024;
	static constexpr char kSentinel = static_cast<char>(0xAA);

	std::vector<char> dest_buffer;
	std::vector<char> expected_buffer;

	void SetUp() override {
		dest_buffer.resize(kBufferSize, kSentinel);
		expected_buffer.resize(kBufferSize, kSentinel);
	}

	void InitializeDestBuffers(const char *base_string) {
		std::fill(dest_buffer.begin(), dest_buffer.end(), kSentinel);
		std::fill(expected_buffer.begin(), expected_buffer.end(), kSentinel);

		std::strcpy(dest_buffer.data() + 100, base_string);
		std::strcpy(expected_buffer.data() + 100, base_string);
	}
};

TEST_F(StringConcatTest, StrcatBasicAppend) {
	InitializeDestBuffers("Erebus");

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;
	const char *src = " OS Kernel";

	klib::strcat(dest, src);
	std::strcat(expected, src);

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_STREQ(dest, "Erebus OS Kernel");

	EXPECT_EQ(dest[16], '\0');
	EXPECT_EQ(dest[17], kSentinel);
}

TEST_F(StringConcatTest, StrcatEmptySource) {
	InitializeDestBuffers("Kernel");

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;

	klib::strcat(dest, "");
	std::strcat(expected, "");

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_STREQ(dest, "Kernel");
}

TEST_F(StringConcatTest, StrncatTruncatesWithMandatoryNullTerminator) {
	InitializeDestBuffers("System ");

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;
	const char *src = "Initialization";

	klib::strncat(dest, src, 4);
	std::strncat(expected, src, 4);

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_STREQ(dest, "System Init");

	EXPECT_EQ(dest[11], '\0');
	EXPECT_EQ(dest[12], kSentinel);
}

TEST_F(StringConcatTest, StrncatNoZeroPaddingTrap) {
	InitializeDestBuffers("Hello ");

	char *dest = dest_buffer.data() + 100;
	char *expected = expected_buffer.data() + 100;
	const char *src = "World";

	klib::strncat(dest, src, 64);
	std::strncat(expected, src, 64);

	EXPECT_EQ(dest_buffer, expected_buffer);
	EXPECT_STREQ(dest, "Hello World");

	EXPECT_EQ(dest[11], '\0');
	EXPECT_EQ(dest[12], kSentinel);
}

TEST_F(StringConcatTest, StrncatZeroCountDoesNothing) {
	InitializeDestBuffers("Erebus");

	char *dest = dest_buffer.data() + 100;

	char *ret = klib::strncat(dest, " Kernel", 0);

	EXPECT_EQ(ret, dest);
	EXPECT_STREQ(dest, "Erebus");
}
