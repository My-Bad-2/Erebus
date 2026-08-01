#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/string/strchr.cpp"

class StringSearchTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufferSize = 4096;
	static constexpr char kSentinel = static_cast<char>(0xAA);

	std::vector<char> buffer;

	void SetUp() override { buffer.resize(kBufferSize, kSentinel); }

	void ResetBuffer() { std::fill(buffer.begin(), buffer.end(), kSentinel); }
};

TEST_F(StringSearchTest, StrchrBasicFind) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Erebus OS Kernel");

	EXPECT_EQ(klibc::strchr(str, 'b'), str + 3);
	EXPECT_EQ(klibc::strchr(str, 'K'), str + 10);
	EXPECT_EQ(klibc::strchr(str, 'Z'), nullptr);
}

TEST_F(StringSearchTest, StrchrNullTerminatorTarget) {
	char *str = buffer.data() + 128;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strchr(str, '\0'), str + 6);
}

TEST_F(StringSearchTest, StrchrPrecedingGarbageTrap) {
	char *aligned_base = buffer.data() + 128;
	char *str = aligned_base + 3;

	std::strcpy(str, "Test");

	aligned_base[1] = 's';
	EXPECT_EQ(klibc::strchr(str, 's'), str + 2);
}

TEST_F(StringSearchTest, StrrchrBasicFind) {
	char *str = buffer.data() + 100;
	std::strcpy(str, "Erebus OS Kernel");

	EXPECT_EQ(klibc::strrchr(str, 'e'), str + 14);
	EXPECT_EQ(klibc::strrchr(str, 'E'), str + 0);
	EXPECT_EQ(klibc::strrchr(str, 'O'), str + 7);
	EXPECT_EQ(klibc::strrchr(str, 'Z'), nullptr);
}

TEST_F(StringSearchTest, StrrchrNullTerminatorTarget) {
	char *str = buffer.data() + 128;
	std::strcpy(str, "Kernel");

	EXPECT_EQ(klibc::strrchr(str, '\0'), str + 6);
}

TEST_F(StringSearchTest, ExhaustiveSizesAndAlignments) {
	const std::vector<std::size_t> lengths = {
			0,	1,	2,	3,	 4,		5, 6, 7, // Unaligned chunks
			8,	9,	15, 16, // Single block bounds
			31, 32, 33, // 32-byte loop bounds
			63, 64, 65, 127, 256,
	};

	const std::vector<std::size_t> alignments = {0, 1, 3, 7};
	const std::size_t anchor = 512;

	for (const std::size_t len: lengths) {
		for (const std::size_t align: alignments) {
			ResetBuffer();

			char *str = buffer.data() + anchor + align;
			std::memset(str, 'A', len);
			str[len] = '\0';

			if (len >= 2) {
				str[0] = 'T';
				str[len / 2] = 'T';
				str[len - 1] = 'T';

				EXPECT_EQ(klibc::strchr(str, 'T'), str + 0) << "strchr failed at len: " << len << ", align: " << align;

				EXPECT_EQ(klibc::strrchr(str, 'T'), str + (len - 1))
						<< "strrchr failed at len: " << len << ", align: " << align;
			}
		}
	}
}
