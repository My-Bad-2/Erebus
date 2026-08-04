#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <string>

#include "../../src/stdlib/atoi.cpp"

class AtoiTest : public ::testing::Test {};

TEST_F(AtoiTest, BasicParsingEquivalence) {
	const char *test_cases[] = {
			"0",					"12345", "-98765", "+42",
			"2147483647", // INT_MAX
			"-2147483648" // INT_MIN
	};

	for (const char *str: test_cases) {
		EXPECT_EQ(klib::atoi(str), std::atoi(str)) << "Failed on: " << str;
		EXPECT_EQ(klib::atol(str), std::atol(str)) << "Failed on: " << str;
		EXPECT_EQ(klib::atoll(str), std::atoll(str)) << "Failed on: " << str;
	}
}

TEST_F(AtoiTest, WhitespaceAndSignHandling) {
	const char *test_cases[] = {
			"   42", "\t\n\r\v\f 42", "   -42", "   +42",
			" - 42", // Invalid space after sign (should return 0)
			" + 42", // Invalid space after sign (should return 0)
			" +-42", // Invalid double sign (should return 0)
			"++42" // Invalid double sign (should return 0)
	};

	for (const char *str: test_cases) {
		EXPECT_EQ(klib::atoi(str), std::atoi(str)) << "Failed on: " << str;
	}
}

TEST_F(AtoiTest, GarbageSuffixHandling) {
	const char *test_cases[] = {
			"123abc456", // Should stop at 'a', returning 123
			"-999.99", // Should stop at '.', returning -999
			"42 ", // Should stop at space, returning 42
			"   100K", // Should stop at 'K', returning 100
			"abcd", // Should stop immediately, returning 0
			"" // Empty string, returning 0
	};

	for (const char *str: test_cases) {
		EXPECT_EQ(klib::atoi(str), std::atoi(str)) << "Failed on: " << str;
	}
}

TEST_F(AtoiTest, AtollAbsoluteLimits) {
	const char *max_ll = "9223372036854775807";
	const char *min_ll = "-9223372036854775808";

	EXPECT_EQ(klib::atoll(max_ll), std::numeric_limits<long long>::max());
	EXPECT_EQ(klib::atoll(min_ll), std::numeric_limits<long long>::min());
}
