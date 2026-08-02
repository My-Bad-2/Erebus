#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <string_view>

#include "../include/kformat.hpp"

class KFormatTest : public ::testing::Test {
protected:
	static constexpr std::size_t kBufSize = 128;
	char buf[kBufSize];

	void SetUp() override { std::memset(buf, 0xAA, kBufSize); }

	void ExpectNoOverflow(std::size_t written_len) const {
		if (written_len < kBufSize) {
			EXPECT_EQ(static_cast<unsigned char>(buf[written_len + 1]), 0xAA)
					<< "Buffer overflow detected past the null terminator!";
		}
	}
};

TEST_F(KFormatTest, BasicStrings) {
	klibc::kprint(buf, sizeof(buf), "Hello, {}!", "Kernel");
	EXPECT_STREQ(buf, "Hello, Kernel!");

	klibc::kprint(buf, sizeof(buf), "Pointer is {}", static_cast<const char *>(nullptr));
	EXPECT_STREQ(buf, "Pointer is (null)");
}

TEST_F(KFormatTest, CharacterFormatting) {
	klibc::kprint(buf, sizeof(buf), "Chars: {}, {}, {}", 'A', 'b', 'C');
	EXPECT_STREQ(buf, "Chars: A, b, C");
}

TEST_F(KFormatTest, IntegersPositiveAndNegative) {
	klibc::kprint(buf, sizeof(buf), "Values: {}, {}", 42, -99);
	EXPECT_STREQ(buf, "Values: 42, -99");
}

TEST_F(KFormatTest, IntegerLimits) {
	// Tests the Two's Complement UB-Free negation logic
	klibc::kprint(buf, sizeof(buf), "Min: {}, Max: {}", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());

	EXPECT_STREQ(buf, "Min: -2147483648, Max: 2147483647");
}

TEST_F(KFormatTest, LongLongLimits) {
	klibc::kprint(buf, sizeof(buf), "LL Min: {}", std::numeric_limits<long long>::min());
	EXPECT_STREQ(buf, "LL Min: -9223372036854775808");
}

TEST_F(KFormatTest, HexadecimalFormatting) {
	klibc::kprint(buf, sizeof(buf), "Lower: {:x}, Upper: {:X}", 255, 255);
	EXPECT_STREQ(buf, "Lower: ff, Upper: FF");

	klibc::kprint(buf, sizeof(buf), "Neg Hex: {:x}", -1);
	EXPECT_STREQ(buf, "Neg Hex: ffffffff");
}

TEST_F(KFormatTest, BinaryAndOctalFormatting) {
	klibc::kprint(buf, sizeof(buf), "Bin: {:b}, Oct: {:o}", 42, 42);
	EXPECT_STREQ(buf, "Bin: 101010, Oct: 52");
}

TEST_F(KFormatTest, ZeroPadding) {
	klibc::kprint(buf, sizeof(buf), "{:05}", 42);
	EXPECT_STREQ(buf, "00042");

	// Padding must account for the negative sign correctly!
	klibc::kprint(buf, sizeof(buf), "{:08}", -123);
	EXPECT_STREQ(buf, "-0000123");
}

TEST_F(KFormatTest, HexZeroPadding) {
	klibc::kprint(buf, sizeof(buf), "MAC: {:02X}:{:02X}:{:02X}", 0x0, 0x1A, 0xFF);
	EXPECT_STREQ(buf, "MAC: 00:1A:FF");
}

TEST_F(KFormatTest, PointerFormatting) {
	int x = 42;
	void *ptr = &x;
	klibc::kprint(buf, sizeof(buf), "Ptr: {:p}", ptr);

	std::string_view result(buf);
	EXPECT_TRUE(result.starts_with("Ptr: 0x"));
	EXPECT_TRUE(result.length() == 5 + 2 + (sizeof(void *) * 2));
}

TEST_F(KFormatTest, NullPointerFormatting) {
	void *ptr = nullptr;
	klibc::kprint(buf, sizeof(buf), "Null: {:p}", ptr);
	EXPECT_STREQ(buf, "Null: 0x0");
}

TEST_F(KFormatTest, BraceEscaping) {
	klibc::kprint(buf, sizeof(buf), "JSON: {{ \"key\": {} }}", 42);
	EXPECT_STREQ(buf, "JSON: { \"key\": 42 }");
}

struct IPv4 {
	uint8_t a, b, c, d;
};

template<>
struct klibc::formatter<IPv4> {
	static constexpr void format(klibc::StringBuffer &buf, const IPv4 &ip, const klibc::FormatSpec &) noexcept {
		FormatSpec num_spec;
		formatter<uint8_t>::format(buf, ip.a, num_spec);
		buf.push('.');
		formatter<uint8_t>::format(buf, ip.b, num_spec);
		buf.push('.');
		formatter<uint8_t>::format(buf, ip.c, num_spec);
		buf.push('.');
		formatter<uint8_t>::format(buf, ip.d, num_spec);
	}
};

TEST_F(KFormatTest, CustomTypeFormatting) {
	IPv4 localhost{127, 0, 0, 1};
	klibc::kprint(buf, sizeof(buf), "Bind to {}", localhost);
	EXPECT_STREQ(buf, "Bind to 127.0.0.1");
}

TEST_F(KFormatTest, ExactBoundaryFit) {
	char tiny[6];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klibc::kprint(tiny, sizeof(tiny), "{}", "Hello");

	EXPECT_EQ(needed, 5);
	EXPECT_STREQ(tiny, "Hello");
}

TEST_F(KFormatTest, StrictTruncation) {
	char tiny[6];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klibc::kprint(tiny, sizeof(tiny), "Kernel Panic");

	EXPECT_EQ(needed, 12);
	EXPECT_STREQ(tiny, "Kerne");
}

TEST_F(KFormatTest, ZeroSizeBufferSafeguard) {
	char tiny[1] = {'X'};

	std::size_t needed = klibc::kprint(tiny, 0, "QuerySize: {}", 42);

	EXPECT_EQ(needed, 13);
	EXPECT_EQ(tiny[0], 'X');
}

TEST_F(KFormatTest, CompileTimeEvaluation) {
	constexpr auto format_at_compile_time = []() {
		char cbuf[32] = {};
		klibc::kprint(cbuf, sizeof(cbuf), "CPU: {} Cores", 8);
		return cbuf[5];
	};

	static_assert(format_at_compile_time() == '8', "Constexpr formatting failed");
	EXPECT_TRUE(true);
}

TEST_F(KFormatTest, StringAlignment) {
	// Strings Left Align by Default
	klibc::kprint(buf, sizeof(buf), "|{:10}|", "Erebus");
	EXPECT_STREQ(buf, "|Erebus    |");

	// Right Align
	klibc::kprint(buf, sizeof(buf), "|{:>10}|", "Erebus");
	EXPECT_STREQ(buf, "|    Erebus|");

	// Center Align with Fill Character
	klibc::kprint(buf, sizeof(buf), "|{:*^10}|", "OS");
	EXPECT_STREQ(buf, "|****OS****|");
}

TEST_F(KFormatTest, NumberAlignment) {
	// Numbers Right Align by Default
	klibc::kprint(buf, sizeof(buf), "|{:10}|", 42);
	EXPECT_STREQ(buf, "|        42|");

	// Left Align
	klibc::kprint(buf, sizeof(buf), "|{:<10}|", 42);
	EXPECT_STREQ(buf, "|42        |");

	// Custom Fill Right Align
	klibc::kprint(buf, sizeof(buf), "|{:.>10}|", 42);
	EXPECT_STREQ(buf, "|........42|");
}

TEST_F(KFormatTest, NegativeNumberAlignment) {
	klibc::kprint(buf, sizeof(buf), "|{:^10}|", -99);
	EXPECT_STREQ(buf, "|   -99    |");

	klibc::kprint(buf, sizeof(buf), "|{:<10}|", -99);
	EXPECT_STREQ(buf, "|-99       |");
}

TEST_F(KFormatTest, BooleanFormatting) {
	klibc::kprint(buf, sizeof(buf), "Kernel Ready: {}, Panic: {}", true, false);
	EXPECT_STREQ(buf, "Kernel Ready: true, Panic: false");
}

TEST_F(KFormatTest, StringViewZeroCopy) {
	std::string_view huge_string = "THIS_IS_A_MASSIVE_STRING";
	std::string_view slice = huge_string.substr(10, 7);

	klibc::kprint(buf, sizeof(buf), "Target: {}", slice);
	EXPECT_STREQ(buf, "Target: MASSIVE");
}

TEST_F(KFormatTest, AlternateFormPrefixes) {
	klibc::kprint(buf, sizeof(buf), "Hex: {:#x}, Bin: {:#b}, Oct: {:#o}", 255, 5, 8);
	EXPECT_STREQ(buf, "Hex: 0xff, Bin: 0b101, Oct: 010");
}

TEST_F(KFormatTest, AlternateFormZeroPadding) {
	klibc::kprint(buf, sizeof(buf), "{:#010x}", 0xABCD);
	EXPECT_STREQ(buf, "0x0000abcd");

	klibc::kprint(buf, sizeof(buf), "{:#10x}", 0xABCD);
	EXPECT_STREQ(buf, "    0xabcd");
}

TEST_F(KFormatTest, AlternateFormZeroBypass) {
	klibc::kprint(buf, sizeof(buf), "{:#x}", 0);
	EXPECT_STREQ(buf, "0");
}

TEST_F(KFormatTest, MemoryHexDump) {
	std::uint8_t memory_block[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};

	klibc::kprint(buf, sizeof(buf), "Block: {:X}", klibc::hexdump{memory_block, 6});

	EXPECT_STREQ(buf, "Block: [DE AD BE EF 00 42]");
}

TEST_F(KFormatTest, DynamicWidthOverride) {
	int column_width = 15;

	klibc::kprint(buf, sizeof(buf), "|{:>}|", klibc::dyn("HazelOS", column_width));

	EXPECT_STREQ(buf, "|        HazelOS|");
}

TEST_F(KFormatTest, ChainedDynamicWidths) {
	int pid_width = 5;
	int mem_width = 8;

	klibc::kprint(buf, sizeof(buf), "[{:<}] [{:0>}]", klibc::dyn("init", pid_width), klibc::dyn(4096, mem_width));

	EXPECT_STREQ(buf, "[init ] [00004096]");
}

TEST_F(KFormatTest, ZeroTempBoundaryTruncation) {
	char tiny[8];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klibc::kprint(tiny, sizeof(tiny), "{}", 123456789);

	EXPECT_EQ(needed, 9);
	EXPECT_STREQ(tiny, "1234567");
}

TEST_F(KFormatTest, AnsiColorAndStyle) {
	klibc::kprint(buf, sizeof(buf), "{}",
								klibc::styled("ERROR", klibc::fg::red, klibc::bg::black, klibc::text_style::bold));

	EXPECT_STREQ(buf, "\x1b[1;31;40mERROR\x1b[0m");
}

TEST_F(KFormatTest, AnsiSingleColorBypass) {
	klibc::kprint(buf, sizeof(buf), "{}", klibc::styled("WARN", klibc::fg::yellow));
	EXPECT_STREQ(buf, "\x1b[33mWARN\x1b[0m");
}

struct CountingSink {
	std::size_t count = 0;

	void push(char /*unused*/) { count++; }
	void append(const std::string_view sv) { count += sv.length(); }

	char *advance(const std::size_t /* unused */) { return nullptr; }
};

TEST_F(KFormatTest, GenericSinkFormatting) {
	CountingSink counter;
	klibc::format_to(counter, "Calculating {} + {} = {}", 5, 10, 15);

	// "Calculating 5 + 10 = 15" -> exactly 23 characters
	EXPECT_EQ(counter.count, 23);
}
