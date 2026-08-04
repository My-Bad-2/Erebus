#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strtok.cpp"

class StringTokenizeTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, 0); }
};

TEST_F(StringTokenizeTest, StrtokRBasicTokenization) {
	char *str = buffer.data();
	std::strcpy(str, "Erebus OS:Fast,Secure kernel");

	char *saveptr = nullptr;

	EXPECT_STREQ(klib::strtok_r(str, " :,", &saveptr), "Erebus");
	EXPECT_STREQ(klib::strtok_r(nullptr, " :,", &saveptr), "OS");
	EXPECT_STREQ(klib::strtok_r(nullptr, " :,", &saveptr), "Fast");
	EXPECT_STREQ(klib::strtok_r(nullptr, " :,", &saveptr), "Secure");
	EXPECT_STREQ(klib::strtok_r(nullptr, " :,", &saveptr), "kernel");

	EXPECT_EQ(klib::strtok_r(nullptr, " :,", &saveptr), nullptr);
}

TEST_F(StringTokenizeTest, StrtokRConsecutiveAndEdgeDelimiters) {
	char *str = buffer.data();
	std::strcpy(str, ",,,apple,,,banana,,cherry,,,");

	char *saveptr = nullptr;

	EXPECT_STREQ(klib::strtok_r(str, ",", &saveptr), "apple");
	EXPECT_STREQ(klib::strtok_r(nullptr, ",", &saveptr), "banana");
	EXPECT_STREQ(klib::strtok_r(nullptr, ",", &saveptr), "cherry");

	EXPECT_EQ(klib::strtok_r(nullptr, ",", &saveptr), nullptr);
}

TEST_F(StringTokenizeTest, StrtokREmptyAndNoDelims) {
	char *saveptr = nullptr;

	char *str1 = buffer.data();
	std::strcpy(str1, "");
	EXPECT_EQ(klib::strtok_r(str1, " ", &saveptr), nullptr);

	char *str2 = buffer.data() + 100;
	std::strcpy(str2, "   ,,,   ");
	EXPECT_EQ(klib::strtok_r(str2, " ,", &saveptr), nullptr);

	char *str3 = buffer.data() + 200;
	std::strcpy(str3, "Kernel");
	EXPECT_STREQ(klib::strtok_r(str3, " ,", &saveptr), "Kernel");
	EXPECT_EQ(klib::strtok_r(nullptr, " ,", &saveptr), nullptr);
}

TEST_F(StringTokenizeTest, StrtokRAllowsInterleavedParsing) {
	char *str1 = buffer.data();
	std::strcpy(str1, "A-B-C");

	char *str2 = buffer.data() + 100;
	std::strcpy(str2, "1:2:3");

	char *saveptr1 = nullptr;
	char *saveptr2 = nullptr;

	EXPECT_STREQ(klib::strtok_r(str1, "-", &saveptr1), "A");
	EXPECT_STREQ(klib::strtok_r(str2, ":", &saveptr2), "1");

	EXPECT_STREQ(klib::strtok_r(nullptr, "-", &saveptr1), "B");
	EXPECT_STREQ(klib::strtok_r(nullptr, ":", &saveptr2), "2");

	EXPECT_STREQ(klib::strtok_r(nullptr, "-", &saveptr1), "C");
	EXPECT_STREQ(klib::strtok_r(nullptr, ":", &saveptr2), "3");
}

TEST_F(StringTokenizeTest, StrtokLegacyWrapperWorks) {
	char *str = buffer.data();
	std::strcpy(str, "Boot Init Kernel");

	EXPECT_STREQ(klib::strtok(str, " "), "Boot");
	EXPECT_STREQ(klib::strtok(nullptr, " "), "Init");
	EXPECT_STREQ(klib::strtok(nullptr, " "), "Kernel");
	EXPECT_EQ(klib::strtok(nullptr, " "), nullptr);
}
