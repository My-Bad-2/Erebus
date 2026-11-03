#include "kformat.hpp"

#include <stdlib.h>
#include <string.h>

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "ktest.hpp"

// The tests live at namespace scope so that they can specialize klib::formatter.
namespace {
// Formatting output; refilled with 0xAA, which no expected output contains, before every test
// that cares.
char buf[128];
}  // namespace

KTEST(KFormat, basic_strings) {
  klib::kprint(buf, sizeof(buf), "Hello, {}!", "Kernel");
  KEXPECT_STREQ(buf, "Hello, Kernel!");

  klib::kprint(buf, sizeof(buf), "Pointer is {}", static_cast<const char*>(nullptr));
  KEXPECT_STREQ(buf, "Pointer is (null)");
}

KTEST(KFormat, character_formatting) {
  klib::kprint(buf, sizeof(buf), "Chars: {}, {}, {}", 'A', 'b', 'C');
  KEXPECT_STREQ(buf, "Chars: A, b, C");
}

KTEST(KFormat, integers_positive_and_negative) {
  klib::kprint(buf, sizeof(buf), "Values: {}, {}", 42, -99);
  KEXPECT_STREQ(buf, "Values: 42, -99");
}

KTEST(KFormat, integer_limits) {
  // Tests the Two's Complement UB-Free negation logic
  klib::kprint(buf, sizeof(buf), "Min: {}, Max: {}", std::numeric_limits<int>::min(),
               std::numeric_limits<int>::max());

  KEXPECT_STREQ(buf, "Min: -2147483648, Max: 2147483647");
}

KTEST(KFormat, long_long_limits) {
  klib::kprint(buf, sizeof(buf), "LL Min: {}", std::numeric_limits<long long>::min());
  KEXPECT_STREQ(buf, "LL Min: -9223372036854775808");
}

KTEST(KFormat, hexadecimal_formatting) {
  klib::kprint(buf, sizeof(buf), "Lower: {:x}, Upper: {:X}", 255, 255);
  KEXPECT_STREQ(buf, "Lower: ff, Upper: FF");

  // As in std::format, negative numbers keep their sign in every base.
  klib::kprint(buf, sizeof(buf), "Neg Hex: {:x}", -26);
  KEXPECT_STREQ(buf, "Neg Hex: -1a");

  klib::kprint(buf, sizeof(buf), "Unsigned: {:x}", 0xFFFF'FFFFu);
  KEXPECT_STREQ(buf, "Unsigned: ffffffff");
}

KTEST(KFormat, binary_and_octal_formatting) {
  klib::kprint(buf, sizeof(buf), "Bin: {:b}, Oct: {:o}", 42, 42);
  KEXPECT_STREQ(buf, "Bin: 101010, Oct: 52");
}

KTEST(KFormat, zero_padding) {
  klib::kprint(buf, sizeof(buf), "{:05}", 42);
  KEXPECT_STREQ(buf, "00042");

  // Padding must account for the negative sign correctly!
  klib::kprint(buf, sizeof(buf), "{:08}", -123);
  KEXPECT_STREQ(buf, "-0000123");
}

KTEST(KFormat, hex_zero_padding) {
  klib::kprint(buf, sizeof(buf), "MAC: {:02X}:{:02X}:{:02X}", 0x0, 0x1A, 0xFF);
  KEXPECT_STREQ(buf, "MAC: 00:1A:FF");
}

KTEST(KFormat, pointer_formatting) {
  int x = 42;
  void* ptr = &x;
  klib::kprint(buf, sizeof(buf), "Ptr: {:p}", ptr);

  const std::string_view result(buf);
  KREQUIRE(result.starts_with("Ptr: 0x"));
  KEXPECT_EQ(klib::strtoull(buf + 7, nullptr, 16), reinterpret_cast<std::uintptr_t>(ptr));
}

KTEST(KFormat, null_pointer_formatting) {
  void* ptr = nullptr;
  klib::kprint(buf, sizeof(buf), "Null: {:p}", ptr);
  KEXPECT_STREQ(buf, "Null: (nil)");
}

KTEST(KFormat, brace_escaping) {
  klib::kprint(buf, sizeof(buf), "JSON: {{ \"key\": {} }}", 42);
  KEXPECT_STREQ(buf, "JSON: { \"key\": 42 }");
}

struct IPv4 {
  uint8_t a, b, c, d;
};

template <>
struct klib::formatter<IPv4> {
  static constexpr void format(klib::StringBuffer& buf, const IPv4& ip,
                               const klib::FormatSpec&) noexcept {
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

KTEST(KFormat, custom_type_formatting) {
  IPv4 localhost{127, 0, 0, 1};
  klib::kprint(buf, sizeof(buf), "Bind to {}", localhost);
  KEXPECT_STREQ(buf, "Bind to 127.0.0.1");
}

KTEST(KFormat, exact_boundary_fit) {
  char tiny[6];
  klib::memset(tiny, 0xAA, sizeof(tiny));

  std::size_t needed = klib::kprint(tiny, sizeof(tiny), "{}", "Hello");

  KEXPECT_EQ(needed, 5);
  KEXPECT_STREQ(tiny, "Hello");
}

KTEST(KFormat, strict_truncation) {
  char tiny[6];
  klib::memset(tiny, 0xAA, sizeof(tiny));

  std::size_t needed = klib::kprint(tiny, sizeof(tiny), "Kernel Panic");

  KEXPECT_EQ(needed, 12);
  KEXPECT_STREQ(tiny, "Kerne");
}

KTEST(KFormat, zero_size_buffer_safeguard) {
  char tiny[1] = {'X'};

  std::size_t needed = klib::kprint(tiny, 0, "QuerySize: {}", 42);

  KEXPECT_EQ(needed, 13);
  KEXPECT_EQ(tiny[0], 'X');
}

KTEST(KFormat, compile_time_evaluation) {
  constexpr auto format_at_compile_time = []() {
    char cbuf[32] = {};
    klib::kprint(cbuf, sizeof(cbuf), "CPU: {} Cores", 8);
    return cbuf[5];
  };

  static_assert(format_at_compile_time() == '8', "Constexpr formatting failed");
  KEXPECT(true);
}

KTEST(KFormat, string_alignment) {
  // Strings Left Align by Default
  klib::kprint(buf, sizeof(buf), "|{:10}|", "Erebus");
  KEXPECT_STREQ(buf, "|Erebus    |");

  // Right Align
  klib::kprint(buf, sizeof(buf), "|{:>10}|", "Erebus");
  KEXPECT_STREQ(buf, "|    Erebus|");

  // Center Align with Fill Character
  klib::kprint(buf, sizeof(buf), "|{:*^10}|", "OS");
  KEXPECT_STREQ(buf, "|****OS****|");
}

KTEST(KFormat, number_alignment) {
  // Numbers Right Align by Default
  klib::kprint(buf, sizeof(buf), "|{:10}|", 42);
  KEXPECT_STREQ(buf, "|        42|");

  // Left Align
  klib::kprint(buf, sizeof(buf), "|{:<10}|", 42);
  KEXPECT_STREQ(buf, "|42        |");

  // Custom Fill Right Align
  klib::kprint(buf, sizeof(buf), "|{:.>10}|", 42);
  KEXPECT_STREQ(buf, "|........42|");
}

KTEST(KFormat, negative_number_alignment) {
  klib::kprint(buf, sizeof(buf), "|{:^10}|", -99);
  KEXPECT_STREQ(buf, "|   -99    |");

  klib::kprint(buf, sizeof(buf), "|{:<10}|", -99);
  KEXPECT_STREQ(buf, "|-99       |");
}

KTEST(KFormat, boolean_formatting) {
  klib::kprint(buf, sizeof(buf), "Kernel Ready: {}, Panic: {}", true, false);
  KEXPECT_STREQ(buf, "Kernel Ready: true, Panic: false");
}

KTEST(KFormat, string_view_zero_copy) {
  std::string_view huge_string = "THIS_IS_A_MASSIVE_STRING";
  std::string_view slice = huge_string.substr(10, 7);

  klib::kprint(buf, sizeof(buf), "Target: {}", slice);
  KEXPECT_STREQ(buf, "Target: MASSIVE");
}

KTEST(KFormat, alternate_form_prefixes) {
  klib::kprint(buf, sizeof(buf), "Hex: {:#x}, Bin: {:#b}, Oct: {:#o}", 255, 5, 8);
  KEXPECT_STREQ(buf, "Hex: 0xff, Bin: 0b101, Oct: 010");
}

KTEST(KFormat, alternate_form_zero_padding) {
  klib::kprint(buf, sizeof(buf), "{:#010x}", 0xABCD);
  KEXPECT_STREQ(buf, "0x0000abcd");

  klib::kprint(buf, sizeof(buf), "{:#10x}", 0xABCD);
  KEXPECT_STREQ(buf, "    0xabcd");
}

KTEST(KFormat, alternate_form_zero_bypass) {
  klib::kprint(buf, sizeof(buf), "{:#x}", 0);
  KEXPECT_STREQ(buf, "0");
}

KTEST(KFormat, memory_hex_dump) {
  std::uint8_t memory_block[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};

  klib::kprint(buf, sizeof(buf), "Block: {:X}", std::as_bytes(std::span{memory_block}));

  KEXPECT_STREQ(buf, "Block: [DE AD BE EF 00 42]");
}

KTEST(KFormat, dynamic_width_override) {
  int column_width = 15;

  klib::kprint(buf, sizeof(buf), "|{:>}|", klib::dyn("Erebus", column_width));

  KEXPECT_STREQ(buf, "|         Erebus|");
}

KTEST(KFormat, chained_dynamic_widths) {
  int pid_width = 5;
  int mem_width = 8;

  klib::kprint(buf, sizeof(buf), "[{:<}] [{:0>}]", klib::dyn("init", pid_width),
               klib::dyn(4096, mem_width));

  KEXPECT_STREQ(buf, "[init ] [00004096]");
}

KTEST(KFormat, zero_temp_boundary_truncation) {
  char tiny[8];
  klib::memset(tiny, 0xAA, sizeof(tiny));

  std::size_t needed = klib::kprint(tiny, sizeof(tiny), "{}", 123456789);

  KEXPECT_EQ(needed, 9);
  KEXPECT_STREQ(tiny, "1234567");
}

KTEST(KFormat, ansi_color_and_style) {
  klib::kprint(buf, sizeof(buf), "{}",
               klib::styled("ERROR", klib::color::fg::red, klib::color::bg::black,
                            klib::color::style::bold));

  KEXPECT_STREQ(buf, "\x1b[1;31;40mERROR\x1b[0m");
}

KTEST(KFormat, ansi_single_color_bypass) {
  klib::kprint(buf, sizeof(buf), "{}", klib::styled("WARN", klib::color::fg::yellow));
  KEXPECT_STREQ(buf, "\x1b[33mWARN\x1b[0m");
}

// A sink with nothing but push() and append(): padding must still work without append_fill().
struct CountingSink {
  std::size_t count = 0;

  void push(char /*unused*/) noexcept { count++; }
  void append(const std::string_view sv) noexcept { count += sv.length(); }

  char* advance(const std::size_t /* unused */) { return nullptr; }
};

KTEST(KFormat, generic_sink_formatting) {
  CountingSink counter;
  klib::format_to(counter, "Calculating {} + {} = {}", 5, 10, 15);

  // "Calculating 5 + 10 = 15" -> exactly 23 characters
  KEXPECT_EQ(counter.count, 23);
}

namespace {
struct [[gnu::aligned(8)]] TestEntry {
  uint64_t val;
  bool pres;
};
}  // namespace

template <>
struct klib::formatter<TestEntry> {
  template <typename Sink>
  static constexpr void format(Sink& buf, const TestEntry& pte, const FormatSpec& spec) noexcept {
    if (spec.custom == "val") {
      klib::format_to(buf, "{:#010x}", pte.val & ~0xFFFULL);
    } else if (spec.custom == "pres") {
      klib::format_to(buf, "[{}{}]", pte.pres ? 'P' : '-', pte.pres ? 'Y' : 'N');
    }
  }
};

KTEST(KFormat, custom_specifier_dialects) {
  TestEntry val{0x4000 | 0b11, true};

  klib::kprint(buf, sizeof(buf), "Val: {:val}", val);
  KEXPECT_STREQ(buf, "Val: 0x00004000");

  klib::memset(buf, 0, sizeof(buf));
  klib::kprint(buf, sizeof(buf), "Pres: {:pres}", val);
  KEXPECT_STREQ(buf, "Pres: [PY]");
}

KTEST(KFormat, positional_arguments) {
  klib::kprint(buf, sizeof(buf), "Target: {1}, Source: {0}", "SRC", "DST");
  KEXPECT_STREQ(buf, "Target: DST, Source: SRC");

  klib::memset(buf, 0, sizeof(buf));
  klib::kprint(buf, sizeof(buf), "Failed to lock {0}. (Object {0} is busy)", "Mutex_A");
  KEXPECT_STREQ(buf, "Failed to lock Mutex_A. (Object Mutex_A is busy)");
}

KTEST(KFormat, positional_with_specifiers) {
  klib::kprint(buf, sizeof(buf), "{1:#06x} | {0:<10}", "Erebus", 255);
  KEXPECT_STREQ(buf, "0x00ff | Erebus    ");
}
