#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include "../include/bitfield.hpp"
#include "../include/kformat.hpp"

using TestSchema = klib::BitfieldSchema<klib::Bit<"Enable", 0>, klib::Bit<"Interrupt", 3>,
																				klib::Field<"Priority", 4, 3>, klib::Field<"Address", 12, 20>>;

TEST(BitfieldTest, HardwareSizeDeduction) {
	using TinySchema = klib::BitfieldSchema<klib::Bit<"Flag", 7>>;
	EXPECT_EQ(sizeof(TinySchema), 1);

	using SmallSchema = klib::BitfieldSchema<klib::Field<"Data", 0, 15>>;
	EXPECT_EQ(sizeof(SmallSchema), 2);
	EXPECT_EQ(sizeof(TestSchema), 4);

	using SmallReservedSchema = klib::BitfieldSchema<klib::Field<"Data", 0, 15>, klib::Reserved<15, 8>>;
	EXPECT_EQ(sizeof(SmallReservedSchema), 4);

	using LargeSchema = klib::BitfieldSchema<klib::Field<"PhysAddr", 12, 40>>;
	EXPECT_EQ(sizeof(LargeSchema), 8);
}

TEST(BitfieldTest, ChainedBuilderAndGetters) {
	constexpr auto bf = TestSchema()
													.set<"Enable">(1)
													.set<"Priority">(5) // 5 = 0b101
													.set<"Address">(0xABC12);

	EXPECT_EQ(bf.get<"Enable">(), 1);
	EXPECT_EQ(bf.get<"Interrupt">(), 0);
	EXPECT_EQ(bf.get<"Priority">(), 5);
	EXPECT_EQ(bf.get<"Address">(), 0xABC12);

	std::uint32_t expected_raw = (1U << 0) | (5U << 4) | (0xABC12U << 12);
	EXPECT_EQ(static_cast<uint32_t>(bf), expected_raw);
}

TEST(BitfieldTest, MaskTruncationSafety) {
	auto bf = TestSchema().set<"Priority">(0xFFFFFFFF);

	EXPECT_EQ(bf.get<"Priority">(), 7); // Truncated to 0b111

	EXPECT_EQ(static_cast<uint32_t>(bf), (7U << 4));
}

TEST(BitfieldTest, FullWidthEdgeCase) {
	using Full64 = klib::BitfieldSchema<klib::Field<"Raw", 0, 64>>;

	auto bf = Full64().set<"Raw">(0xFFFFFFFFFFFFFFFFULL);
	EXPECT_EQ(bf.get<"Raw">(), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(BitfieldTest, FormattingEngineIntegration) {
	char buf[128];

	auto pte = TestSchema().set<"Enable">(1).set<"Address">(0x4000);
	klib::kprint(buf, sizeof(buf), "{}", pte);
	EXPECT_STREQ(buf, "[Enable | Address=0x4000]");

	std::memset(buf, 0, sizeof(buf));
	auto empty = TestSchema();
	klib::kprint(buf, sizeof(buf), "{}", empty);

	EXPECT_STREQ(buf, "[NONE]");

	std::memset(buf, 0, sizeof(buf));
	klib::kprint(buf, sizeof(buf), "CR3 is {}", TestSchema().set<"Priority">(3));

	EXPECT_STREQ(buf, "CR3 is [Priority=0x3]");
}

using TestReg = klib::BitfieldSchema<klib::Field<"A", 0, 8>,
																		 klib::ReadOnly<"B", 12>,
																		 klib::Field<"C", 17, 2>,
																		 klib::WriteOnly<"D", 20>
																		 >;

TEST(BitfieldTest, CompileTimeMaskGenerator) {
	constexpr auto mask = TestReg::generate_mask<"A", "C">();

	// A = Bits 0-7 (0xFF)
	// C = Bits 17-18 (0x60000)
	// Combined = 0x600FF
	EXPECT_EQ(mask, 0x600FF);
}
