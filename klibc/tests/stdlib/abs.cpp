#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <cinttypes>

#include "../../src/stdlib/abs.cpp"

class StdlibMathTest : public ::testing::Test {};

TEST_F(StdlibMathTest, AbsEquivalence) {
    EXPECT_EQ(klibc::abs(-42), 42);
    EXPECT_EQ(klibc::abs(42), 42);
    EXPECT_EQ(klibc::abs(0), 0);

    EXPECT_EQ(klibc::labs(-99999L), 99999L);
    EXPECT_EQ(klibc::llabs(-9999999999999LL), 9999999999999LL);
    EXPECT_EQ(klibc::imaxabs(-1234567890LL), 1234567890LL);
}

TEST_F(StdlibMathTest, AbsExtremeBoundsWrap) {
    EXPECT_EQ(klibc::abs(std::numeric_limits<int>::min()), std::numeric_limits<int>::min());
    EXPECT_EQ(klibc::llabs(std::numeric_limits<long long>::min()), std::numeric_limits<long long>::min());
}

TEST_F(StdlibMathTest, DivBasicMath) {
    auto d = klibc::div(38, 5);
    EXPECT_EQ(d.quot, 7);
    EXPECT_EQ(d.rem, 3);

    auto ld = klibc::ldiv(999L, 10L);
    EXPECT_EQ(ld.quot, 99L);
    EXPECT_EQ(ld.rem, 9L);

    auto lld = klibc::lldiv(1000000000000LL, 3LL);
    EXPECT_EQ(lld.quot, 333333333333LL);
    EXPECT_EQ(lld.rem, 1LL);
}

TEST_F(StdlibMathTest, DivStandardSignRules) {
    auto d1 = klibc::div(-10, 3);
    EXPECT_EQ(d1.quot, -3);
    EXPECT_EQ(d1.rem, -1);

    auto d2 = klibc::div(10, -3);
    EXPECT_EQ(d2.quot, -3);
    EXPECT_EQ(d2.rem, 1);

    auto d3 = klibc::div(-10, -3);
    EXPECT_EQ(d3.quot, 3);
    EXPECT_EQ(d3.rem, -1);
}
