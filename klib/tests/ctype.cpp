#include <cctype>
#include <gtest/gtest.h>

#define CTYPE_TEST
#include "../include/ctype.h"

class CTypeTest : public ::testing::Test {
protected:
	static bool Normalize(int val) { return !!val; }
};

TEST_F(CTypeTest, ExhaustiveIsLower) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::islower(i)), Normalize(std::islower(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsUpper) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isupper(i)), Normalize(std::isupper(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsAlpha) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isalpha(i)), Normalize(std::isalpha(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsDigit) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isdigit(i)), Normalize(std::isdigit(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsAlnum) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isalnum(i)), Normalize(std::isalnum(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsXDigit) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isxdigit(i)), Normalize(std::isxdigit(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsSpace) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isspace(i)), Normalize(std::isspace(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsBlank) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isblank(i)), Normalize(std::isblank(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsPrint) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isprint(i)), Normalize(std::isprint(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsGraph) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::isgraph(i)), Normalize(std::isgraph(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsCntrl) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::iscntrl(i)), Normalize(std::iscntrl(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveIsPunct) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(Normalize(klib::ispunct(i)), Normalize(std::ispunct(i))) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveToLower) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(klib::tolower(i), std::tolower(i)) << "Failed at: " << i;
	}
}

TEST_F(CTypeTest, ExhaustiveToUpper) {
	for (int i = -1; i <= 255; ++i) {
		EXPECT_EQ(klib::toupper(i), std::toupper(i)) << "Failed at: " << i;
	}
}

#undef CTYPE_TEST
