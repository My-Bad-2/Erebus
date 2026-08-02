#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>

#include "../../src/stdlib/strtol.cpp"

class StrToXTest : public ::testing::Test {};

TEST_F(StrToXTest, Base10AndSign) {
	char *end;
	EXPECT_EQ(klibc::strtol("   -123456", &end, 10), -123456);
	EXPECT_EQ(*end, '\0');

	EXPECT_EQ(klibc::strtol("+42 is the answer", &end, 10), 42);
	EXPECT_EQ(*end, ' ');
}

TEST_F(StrToXTest, Base16Explicit) {
	char *end;
	EXPECT_EQ(klibc::strtol("0x1A", &end, 16), 26);
	EXPECT_EQ(klibc::strtol("-FF", &end, 16), -255);
}

TEST_F(StrToXTest, BaseInferenceMode) {
	char *end;
	EXPECT_EQ(klibc::strtol("0x20", &end, 0), 32);
	EXPECT_EQ(klibc::strtol("010", &end, 0), 8);
	EXPECT_EQ(klibc::strtol("99", &end, 0), 99);
}

TEST_F(StrToXTest, TheZeroXRollbackTrap) {
	char *end = nullptr;

	EXPECT_EQ(klibc::strtol("0xZ", &end, 16), 0);
	EXPECT_EQ(*end, 'x');

	EXPECT_EQ(klibc::strtol("0xZ", &end, 0), 0);
	EXPECT_EQ(*end, 'x');
}

TEST_F(StrToXTest, EmptyStringAndNoDigits) {
	const char *str = "   +Z";
	char *end = nullptr;

	EXPECT_EQ(klibc::strtol(str, &end, 10), 0);
	EXPECT_EQ(end, str);
}

TEST_F(StrToXTest, StrtoulNegativeWrapMandate) {
	char *end;

	EXPECT_EQ(klibc::strtoul("-1", &end, 10), std::numeric_limits<unsigned long>::max());
	EXPECT_EQ(klibc::strtoul("-2", &end, 10), std::numeric_limits<unsigned long>::max() - 1);
}

TEST_F(StrToXTest, StrtolOverflowSaturation) {
	char *end;

	EXPECT_EQ(klibc::strtoll("9999999999999999999999999", &end, 10), std::numeric_limits<long long>::max());
	EXPECT_EQ(klibc::strtoll("-9999999999999999999999999", &end, 10), std::numeric_limits<long long>::min());
}

TEST_F(StrToXTest, StrtoullOverflowSaturation) {
	char *end;

	EXPECT_EQ(klibc::strtoull("9999999999999999999999999", &end, 10), std::numeric_limits<unsigned long long>::max());
	EXPECT_EQ(klibc::strtoull("-9999999999999999999999999", &end, 10), std::numeric_limits<unsigned long long>::max());
}
