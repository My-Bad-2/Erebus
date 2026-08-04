#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <string_view>

#include "../include/fixed_point.hpp"
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
	klib::kprint(buf, sizeof(buf), "Hello, {}!", "Kernel");
	EXPECT_STREQ(buf, "Hello, Kernel!");

	klib::kprint(buf, sizeof(buf), "Pointer is {}", static_cast<const char *>(nullptr));
	EXPECT_STREQ(buf, "Pointer is (null)");
}

TEST_F(KFormatTest, CharacterFormatting) {
	klib::kprint(buf, sizeof(buf), "Chars: {}, {}, {}", 'A', 'b', 'C');
	EXPECT_STREQ(buf, "Chars: A, b, C");
}

TEST_F(KFormatTest, IntegersPositiveAndNegative) {
	klib::kprint(buf, sizeof(buf), "Values: {}, {}", 42, -99);
	EXPECT_STREQ(buf, "Values: 42, -99");
}

TEST_F(KFormatTest, IntegerLimits) {
	// Tests the Two's Complement UB-Free negation logic
	klib::kprint(buf, sizeof(buf), "Min: {}, Max: {}", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());

	EXPECT_STREQ(buf, "Min: -2147483648, Max: 2147483647");
}

TEST_F(KFormatTest, LongLongLimits) {
	klib::kprint(buf, sizeof(buf), "LL Min: {}", std::numeric_limits<long long>::min());
	EXPECT_STREQ(buf, "LL Min: -9223372036854775808");
}

TEST_F(KFormatTest, HexadecimalFormatting) {
	klib::kprint(buf, sizeof(buf), "Lower: {:x}, Upper: {:X}", 255, 255);
	EXPECT_STREQ(buf, "Lower: ff, Upper: FF");

	klib::kprint(buf, sizeof(buf), "Neg Hex: {:x}", -1);
	EXPECT_STREQ(buf, "Neg Hex: ffffffff");
}

TEST_F(KFormatTest, BinaryAndOctalFormatting) {
	klib::kprint(buf, sizeof(buf), "Bin: {:b}, Oct: {:o}", 42, 42);
	EXPECT_STREQ(buf, "Bin: 101010, Oct: 52");
}

TEST_F(KFormatTest, ZeroPadding) {
	klib::kprint(buf, sizeof(buf), "{:05}", 42);
	EXPECT_STREQ(buf, "00042");

	// Padding must account for the negative sign correctly!
	klib::kprint(buf, sizeof(buf), "{:08}", -123);
	EXPECT_STREQ(buf, "-0000123");
}

TEST_F(KFormatTest, HexZeroPadding) {
	klib::kprint(buf, sizeof(buf), "MAC: {:02X}:{:02X}:{:02X}", 0x0, 0x1A, 0xFF);
	EXPECT_STREQ(buf, "MAC: 00:1A:FF");
}

TEST_F(KFormatTest, PointerFormatting) {
	int x = 42;
	void *ptr = &x;
	klib::kprint(buf, sizeof(buf), "Ptr: {:p}", ptr);

	std::string_view result(buf);
	EXPECT_TRUE(result.starts_with("Ptr: 0x"));
	EXPECT_TRUE(result.length() == 5 + 2 + (sizeof(void *) * 2));
}

TEST_F(KFormatTest, NullPointerFormatting) {
	void *ptr = nullptr;
	klib::kprint(buf, sizeof(buf), "Null: {:p}", ptr);
	EXPECT_STREQ(buf, "Null: 0x0");
}

TEST_F(KFormatTest, BraceEscaping) {
	klib::kprint(buf, sizeof(buf), "JSON: {{ \"key\": {} }}", 42);
	EXPECT_STREQ(buf, "JSON: { \"key\": 42 }");
}

struct IPv4 {
	uint8_t a, b, c, d;
};

template<>
struct klib::formatter<IPv4> {
	static constexpr void format(klib::StringBuffer &buf, const IPv4 &ip, const klib::FormatSpec &) noexcept {
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
	klib::kprint(buf, sizeof(buf), "Bind to {}", localhost);
	EXPECT_STREQ(buf, "Bind to 127.0.0.1");
}

TEST_F(KFormatTest, ExactBoundaryFit) {
	char tiny[6];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klib::kprint(tiny, sizeof(tiny), "{}", "Hello");

	EXPECT_EQ(needed, 5);
	EXPECT_STREQ(tiny, "Hello");
}

TEST_F(KFormatTest, StrictTruncation) {
	char tiny[6];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klib::kprint(tiny, sizeof(tiny), "Kernel Panic");

	EXPECT_EQ(needed, 12);
	EXPECT_STREQ(tiny, "Kerne");
}

TEST_F(KFormatTest, ZeroSizeBufferSafeguard) {
	char tiny[1] = {'X'};

	std::size_t needed = klib::kprint(tiny, 0, "QuerySize: {}", 42);

	EXPECT_EQ(needed, 13);
	EXPECT_EQ(tiny[0], 'X');
}

TEST_F(KFormatTest, CompileTimeEvaluation) {
	constexpr auto format_at_compile_time = []() {
		char cbuf[32] = {};
		klib::kprint(cbuf, sizeof(cbuf), "CPU: {} Cores", 8);
		return cbuf[5];
	};

	static_assert(format_at_compile_time() == '8', "Constexpr formatting failed");
	EXPECT_TRUE(true);
}

TEST_F(KFormatTest, StringAlignment) {
	// Strings Left Align by Default
	klib::kprint(buf, sizeof(buf), "|{:10}|", "Erebus");
	EXPECT_STREQ(buf, "|Erebus    |");

	// Right Align
	klib::kprint(buf, sizeof(buf), "|{:>10}|", "Erebus");
	EXPECT_STREQ(buf, "|    Erebus|");

	// Center Align with Fill Character
	klib::kprint(buf, sizeof(buf), "|{:*^10}|", "OS");
	EXPECT_STREQ(buf, "|****OS****|");
}

TEST_F(KFormatTest, NumberAlignment) {
	// Numbers Right Align by Default
	klib::kprint(buf, sizeof(buf), "|{:10}|", 42);
	EXPECT_STREQ(buf, "|        42|");

	// Left Align
	klib::kprint(buf, sizeof(buf), "|{:<10}|", 42);
	EXPECT_STREQ(buf, "|42        |");

	// Custom Fill Right Align
	klib::kprint(buf, sizeof(buf), "|{:.>10}|", 42);
	EXPECT_STREQ(buf, "|........42|");
}

TEST_F(KFormatTest, NegativeNumberAlignment) {
	klib::kprint(buf, sizeof(buf), "|{:^10}|", -99);
	EXPECT_STREQ(buf, "|   -99    |");

	klib::kprint(buf, sizeof(buf), "|{:<10}|", -99);
	EXPECT_STREQ(buf, "|-99       |");
}

TEST_F(KFormatTest, BooleanFormatting) {
	klib::kprint(buf, sizeof(buf), "Kernel Ready: {}, Panic: {}", true, false);
	EXPECT_STREQ(buf, "Kernel Ready: true, Panic: false");
}

TEST_F(KFormatTest, StringViewZeroCopy) {
	std::string_view huge_string = "THIS_IS_A_MASSIVE_STRING";
	std::string_view slice = huge_string.substr(10, 7);

	klib::kprint(buf, sizeof(buf), "Target: {}", slice);
	EXPECT_STREQ(buf, "Target: MASSIVE");
}

TEST_F(KFormatTest, AlternateFormPrefixes) {
	klib::kprint(buf, sizeof(buf), "Hex: {:#x}, Bin: {:#b}, Oct: {:#o}", 255, 5, 8);
	EXPECT_STREQ(buf, "Hex: 0xff, Bin: 0b101, Oct: 010");
}

TEST_F(KFormatTest, AlternateFormZeroPadding) {
	klib::kprint(buf, sizeof(buf), "{:#010x}", 0xABCD);
	EXPECT_STREQ(buf, "0x0000abcd");

	klib::kprint(buf, sizeof(buf), "{:#10x}", 0xABCD);
	EXPECT_STREQ(buf, "    0xabcd");
}

TEST_F(KFormatTest, AlternateFormZeroBypass) {
	klib::kprint(buf, sizeof(buf), "{:#x}", 0);
	EXPECT_STREQ(buf, "0");
}

TEST_F(KFormatTest, MemoryHexDump) {
	std::uint8_t memory_block[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};

	klib::kprint(buf, sizeof(buf), "Block: {:X}", klib::hexdump{memory_block, 6});

	EXPECT_STREQ(buf, "Block: [DE AD BE EF 00 42]");
}

TEST_F(KFormatTest, DynamicWidthOverride) {
	int column_width = 15;

	klib::kprint(buf, sizeof(buf), "|{:>}|", klib::dyn("HazelOS", column_width));

	EXPECT_STREQ(buf, "|        HazelOS|");
}

TEST_F(KFormatTest, ChainedDynamicWidths) {
	int pid_width = 5;
	int mem_width = 8;

	klib::kprint(buf, sizeof(buf), "[{:<}] [{:0>}]", klib::dyn("init", pid_width), klib::dyn(4096, mem_width));

	EXPECT_STREQ(buf, "[init ] [00004096]");
}

TEST_F(KFormatTest, ZeroTempBoundaryTruncation) {
	char tiny[8];
	std::memset(tiny, 0xAA, sizeof(tiny));

	std::size_t needed = klib::kprint(tiny, sizeof(tiny), "{}", 123456789);

	EXPECT_EQ(needed, 9);
	EXPECT_STREQ(tiny, "1234567");
}

TEST_F(KFormatTest, AnsiColorAndStyle) {
	klib::kprint(buf, sizeof(buf), "{}", klib::styled("ERROR", klib::fg::red, klib::bg::black, klib::text_style::bold));

	EXPECT_STREQ(buf, "\x1b[1;31;40mERROR\x1b[0m");
}

TEST_F(KFormatTest, AnsiSingleColorBypass) {
	klib::kprint(buf, sizeof(buf), "{}", klib::styled("WARN", klib::fg::yellow));
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
	klib::format_to(counter, "Calculating {} + {} = {}", 5, 10, 15);

	// "Calculating 5 + 10 = 15" -> exactly 23 characters
	EXPECT_EQ(counter.count, 23);
}

namespace {
	struct [[gnu::aligned(8)]] TestEntry {
		uint64_t val;
		bool pres;
	};
} // namespace

template<>
struct klib::formatter<TestEntry> {
	template<typename Sink>
	static constexpr void format(Sink &buf, const TestEntry &pte, const FormatSpec &spec) noexcept {
		if (spec.custom == "val") {
			klib::format_to(buf, "{:#010x}", pte.val & ~0xFFFULL);
		} else if (spec.custom == "pres") {
			klib::format_to(buf, "[{}{}]", pte.pres ? 'P' : '-', pte.pres ? 'Y' : 'N');
		}
	}
};

TEST_F(KFormatTest, CustomSpecifierDialects) {
	TestEntry val{0x4000 | 0b11, true};

	klib::kprint(buf, sizeof(buf), "Val: {:val}", val);
	EXPECT_STREQ(buf, "Val: 0x00004000");

	std::memset(buf, 0, sizeof(buf));
	klib::kprint(buf, sizeof(buf), "Pres: {:pres}", val);
	EXPECT_STREQ(buf, "Pres: [PY]");
}

TEST_F(KFormatTest, PositionalArguments) {
	klib::kprint(buf, sizeof(buf), "Target: {1}, Source: {0}", "SRC", "DST");
	EXPECT_STREQ(buf, "Target: DST, Source: SRC");

	std::memset(buf, 0, sizeof(buf));
	klib::kprint(buf, sizeof(buf), "Failed to lock {0}. (Object {0} is busy)", "Mutex_A");
	EXPECT_STREQ(buf, "Failed to lock Mutex_A. (Object Mutex_A is busy)");
}

TEST_F(KFormatTest, PositionalWithSpecifiers) {
	klib::kprint(buf, sizeof(buf), "{1:#06x} | {0:<10}", "Erebus", 255);
	EXPECT_STREQ(buf, "0x00ff | Erebus    ");
}

TEST(FixedPointTest, UptimeFormatting) {
	char buf[128] = {0};
	uint64_t uptime_ms = 12345; // 12 seconds, 345 ms

	klib::kprint(buf, sizeof(buf), "Uptime: {}s", klib::fixed<1000>(uptime_ms));
	EXPECT_STREQ(buf, "Uptime: 12.345s");

	std::memset(buf, 0, sizeof(buf));
	klib::kprint(buf, sizeof(buf), "Uptime: {:.2f}s", klib::fixed<1000>(uptime_ms));
	EXPECT_STREQ(buf, "Uptime: 12.35s");
}

TEST(FixedPointTest, CarryOverRoundingEdgeCase) {
	char buf[128] = {0};
	uint32_t val = 9999;

	klib::kprint(buf, sizeof(buf), "{:.1f}", klib::fixed<100>(val));
	EXPECT_STREQ(buf, "100.0");
}

TEST(FixedPointTest, HardwareVoltageMath) {
	// Imagine an ADC returns a 12-bit integer where 4096 = 3.3 Volts.
	char buf[128] = {0};
	uint32_t adc_reading = 2048; // Exactly half (1.65 Volts)

	// adc_reading * 3300 gives millivolts.
	klib::kprint(buf, sizeof(buf), "VCORE: {:.2f}V",
								klib::fixed<4096>((adc_reading * 3300) / 1000));

	EXPECT_STREQ(buf, "VCORE: 1.65V");
}

TEST(FixedPointTest, SignedValuesAndWidthPadding) {
	char buf[128] = {0};
	int32_t temp = -456;

	klib::kprint(buf, sizeof(buf), "Temp: {:>5.1f}C", klib::fixed<100>(temp));
	EXPECT_STREQ(buf, "Temp:  -4.6C");
}
