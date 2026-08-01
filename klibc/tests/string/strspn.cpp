#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strspn.cpp"

class StringSpanTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	static constexpr char kSentinel = static_cast<char>(0xAA);

	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, kSentinel); }

	void ResetBuffer() { std::fill(buffer.begin(), buffer.end(), kSentinel); }
};

TEST_F(StringSpanTest, StrspnBasicMatch) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "742 Evergreen Terrace");

	EXPECT_EQ(klibc::strspn(str, "0123456789"), 3); // "742"
	EXPECT_EQ(klibc::strspn(str, " 742"), 4); // "742 "

	EXPECT_EQ(klibc::strspn(str, "Evergreen"), 0);
}

TEST_F(StringSpanTest, StrspnEmptyEdgeCases) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strspn(str, ""), 0); // Empty accept set
	EXPECT_EQ(klibc::strspn("", "Kernel"), 0); // Empty dest string
}

TEST_F(StringSpanTest, StrcspnBasicReject) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "system.config.cfg");

	EXPECT_EQ(klibc::strcspn(str, "."), 6); // stops at '.'
	EXPECT_EQ(klibc::strcspn(str, "f"), 10); // stops at 'f' in config

	EXPECT_EQ(klibc::strcspn(str, "Z"), 17);
}

TEST_F(StringSpanTest, StrcspnEmptyEdgeCases) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strcspn(str, ""), 6);
	EXPECT_EQ(klibc::strcspn("", "Kernel"), 0);
}

TEST_F(StringSpanTest, ExhaustiveSizesAndAlignments) {
	const std::vector<std::size_t> lengths = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 64, 127};
	const std::vector<std::size_t> alignments = {0, 1, 3, 7};
	const std::size_t anchor = 512;

	for (const std::size_t len: lengths) {
		for (const std::size_t align: alignments) {
			ResetBuffer();

			char *str = buffer.data() + anchor + align;
			std::memset(str, 'A', len);
			str[len] = '\0';

			EXPECT_EQ(klibc::strspn(str, "A"), len) << "strspn failed full match";
			EXPECT_EQ(klibc::strcspn(str, "B"), len) << "strcspn failed full bypass";

			if (len >= 2) {
				const std::size_t inject_idx = len / 2;

				str[inject_idx] = 'X';
				EXPECT_EQ(klibc::strspn(str, "AB"), inject_idx);
				str[inject_idx] = 'A';

				str[inject_idx] = 'Y';
				EXPECT_EQ(klibc::strcspn(str, "YZ"), inject_idx);
			}
		}
	}
}
