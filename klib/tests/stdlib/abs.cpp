#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <cinttypes>

#include "../../src/stdlib/abs.cpp"

class StdlibMathTest : public ::testing::Test {};

TEST_F(StdlibMathTest, AbsEquivalence) {
    EXPECT_EQ(klib::abs(-42), 42);
    EXPECT_EQ(klib::abs(42), 42);
    EXPECT_EQ(klib::abs(0), 0);

    EXPECT_EQ(klib::labs(-99999L), 99999L);
    EXPECT_EQ(klib::llabs(-9999999999999LL), 9999999999999LL);
    EXPECT_EQ(klib::imaxabs(-1234567890LL), 1234567890LL);
}

TEST_F(StdlibMathTest, AbsExtremeBoundsWrap) {
    EXPECT_EQ(klib::abs(std::numeric_limits<int>::min()), std::numeric_limits<int>::min());
    EXPECT_EQ(klib::llabs(std::numeric_limits<long long>::min()), std::numeric_limits<long long>::min());
}

TEST_F(StdlibMathTest, DivBasicMath) {
    auto d = klib::div(38, 5);
    EXPECT_EQ(d.quot, 7);
    EXPECT_EQ(d.rem, 3);

    auto ld = klib::ldiv(999L, 10L);
    EXPECT_EQ(ld.quot, 99L);
    EXPECT_EQ(ld.rem, 9L);

    auto lld = klib::lldiv(1000000000000LL, 3LL);
    EXPECT_EQ(lld.quot, 333333333333LL);
    EXPECT_EQ(lld.rem, 1LL);
}

TEST_F(StdlibMathTest, DivStandardSignRules) {
    auto d1 = klib::div(-10, 3);
    EXPECT_EQ(d1.quot, -3);
    EXPECT_EQ(d1.rem, -1);

    auto d2 = klib::div(10, -3);
    EXPECT_EQ(d2.quot, -3);
    EXPECT_EQ(d2.rem, 1);

    auto d3 = klib::div(-10, -3);
    EXPECT_EQ(d3.quot, 3);
    EXPECT_EQ(d3.rem, -1);
}
