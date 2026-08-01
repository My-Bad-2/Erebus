#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strpbrk.cpp"

class AdvancedStringSearchTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	static constexpr char kSentinel = static_cast<char>(0xAA);

	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, kSentinel); }

	void ResetBuffer() { std::fill(buffer.begin(), buffer.end(), kSentinel); }
};

TEST_F(AdvancedStringSearchTest, StrpbrkBasicMatch) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "System Initialization");

	// 'm' is the first character in "System" that matches "zxm"
	EXPECT_EQ(klibc::strpbrk(str, "zxm"), str + 5);

	// 'I' is the first character that matches "I"
	EXPECT_EQ(klibc::strpbrk(str, "I"), str + 7);
}

TEST_F(AdvancedStringSearchTest, StrpbrkNoMatchNullTrap) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strpbrk(str, "Z"), nullptr);
	EXPECT_EQ(klibc::strpbrk(str, ""), nullptr);
}

TEST_F(AdvancedStringSearchTest, StrstrBasicMatch) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Erebus OS Kernel");

	EXPECT_EQ(klibc::strstr(str, "OS"), str + 7);
	EXPECT_EQ(klibc::strstr(str, "Kernel"), str + 10);
	EXPECT_EQ(klibc::strstr(str, "Erebus"), str + 0);
}

TEST_F(AdvancedStringSearchTest, StrstrEmptyNeedleRule) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strstr(str, ""), str);
}

TEST_F(AdvancedStringSearchTest, StrstrLargerNeedleRejection) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Short");

	EXPECT_EQ(klibc::strstr(str, "ShortString"), nullptr);
}

TEST_F(AdvancedStringSearchTest, StrstrTheMississippiTrap) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "mississippi");

	// It must properly backtrack/skip without being confused by repeating characters.
	EXPECT_EQ(klibc::strstr(str, "issip"), str + 4); // "issip" is at index 4
	EXPECT_EQ(klibc::strstr(str, "issi"), str + 1); // The first "issi" is at index 1

	std::strcpy(str, "AAAAAAAAAAAAAAAAAAAAAB");
	EXPECT_EQ(klibc::strstr(str, "AAAAAB"), str + 16);
}

TEST_F(AdvancedStringSearchTest, StrstrBoundsTrap) {
	char *str = buffer.data() + 128;
	std::strcpy(str, "ValidString");

	// Put a partial match exactly at the end of the string.
	// "Strin" matches, but the 'g' does not match 'X'.
	// The `search_bounds` math must prevent `strstr` from reading past '\0'
	// into unmapped memory to find the missing 'X'.
	EXPECT_EQ(klibc::strstr(str, "StringX"), nullptr);
}
