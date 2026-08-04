#set page(
    paper: "a4",
    margin: (x: 1in, y: 1in),
)
#set text(
    font: "New Computer Modern",
    size: 11pt,
)
#set heading(numbering: "1.1")
#set par(justify: true)
#show raw.where(block: true): block.with(
    fill: luma(245),
    inset: 10pt,
    radius: 4pt,
    width: 100%,
)
#show raw: set text(font: "Fira Code", size: 0.95em)

#align(center)[
    #text(size: 22pt, weight: "bold")[Erebus `kformat` engine]
    #v(1em)
]

`kformat` is a C++20, high-performance, zero-allocation string formatting engine designed explicitly for the kernel environment.

= Main APIs

The engine exposes two primary functions:

== `klib::kprint`
Formats a string into a bounded memory array (C strings). It strictly enforces buffer boundaries and guarantees null-termination.

```cpp
char buf[128];
std::size_t written = klib::kprint(buf, sizeof(buf), "Hello {}!", "World");
```

== `klib::format_to`
Formats text directly into any custom `Sink` object.

```cpp
Sink sink;
klib::format_to(sink, "Invalid address at {:#010x}", fault_addr);
```

= Format Specification Syntax

Format strings use `{}` placeholders. Placeholders can contain a format specification mini-language:
`{:[fill][align][#][0][width][type]}`

#align(center)[
    #table(
        columns: (auto, auto, 1fr, auto),
        align: (left, center, left, left),
        stroke: 0.5pt,
        table.header(
            [Component],
            [Syntax],
            [Description],
            [Example],
        ),

        [_Fill & Align_],
        [`char` + `<`, `>`, or `^`],
        [Pads output with a custom character. `<` (Left), `>` (Right), `^` (Center).],
        [`{:*^10}` $->$ `***42***`],

        [_Alternate Form_], [`#`], [Prepends base prefixes (`0x`, `0b`, `0`).], [`{:#x}` $->$ `0xff`],
        [_Zero Pad_], [`0`], [Pads numbers with leading zeros instead of spaces.], [`{:05}` $->$ `00042`],
        [_Width_], [`number`], [The minimum width of the field.], [`{:10}` $->$ `        42`],
        [_Type_], [`x`, `X`, `b`, `o`, `p`], [`x/X`: Hex, `b`: Binary, `o`: Octal, `p`: Pointer.], [`{:X}` $->$ `FF`],
    )
]

= More Modifiers

== Dynamic Widths (`klib::dyn`)

When column widths must be calculated at runtime, wrap the variable in `klib::dyn()`. This overrides the format string's width specifier.

```cpp
int max_width = get_longest_name();
klib::kprint(buf, size, "[{:<}]", klib::dyn(name, max_width));
```

== ANSI Colors & Styling (`klib::styled`)

Apply terminal styling without allocating memory or manipulating raw escape codes.

```cpp
using namespace klib;
format_to(sink, "[{}] System halted.", styled("FATAL", fg::red, bg::black, text_style::bold));
```

- _Foregrounds (`fg::`) / Backgrounds (`bg::`):_ `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, `reset`.
- _Styles (`text_styles::`):_ `none`, `bold`, `dim`, `italic`, `underline`, `blink`.

== Memory Debugging (Hexdump)

When debugging, dumping raw physical or virtual memory is a daily requirement. `kformat` includes a built-in `klib::hexdump` wrapper that formats arbitary memory blocks instantly.

```cpp
unsigned char page_buffer[16] = { /* ... */ };

// Dumps the memory block as a space-separated hex array.
// The `X` specifier forces uppercase hex digits.
klib::kprint(buf, size, "Page Content: {:X}",
              klib::hexdump{page_buffer, sizeof(page_buffer)});

// Output: Page Content: [DE AD BE EF 00 00 00 00 ...]
```

= The Sink Architecture

A `Sink` is an object that consumes characters. By writing formatters to a Sink rather than a pointer, `kformat` can write directly to hardware.

To create a custom Sink, you must implement three methods:

```cpp
struct MySink {
  // push: Write a single character.
  constexpr void push(char c) noexcept;

  // append: Write a bulk string block.
  constexpr void append(std::string_view sv) noexcept;

  // advance: Request a direct memory pointer for N contiguous bytes.
  // Return nullptr if memory isn't contiguous or if the buffer is full. The engine will fallback to push().
  constexpr char* advance(std::size_t n) noexcept;
};
```

= Adding Custom Types (Extensibility)

To make `kformat` understand a custom kernel struct, specialize the `klib::formatter<T>` trait in the `klib` namespace.

== Basic Struct Formatting
```cpp
struct IPv4 { uint8_t a, b, c, d; };

namespace klib {
  template <>
  struct formatter<IPv4> {
    template <typename Sink>
    static constexpr void format(Sink &sink, const IPv4 &ip, const FormatSpec&) noexcept {
      // Re-use existing formatters by creating a blank FormatSpec
      FormatSpec num_spec;

      formatter<uint8_t>::format(sink, ip.a, num_spec);
      sink.push('.');
      formatter<uint8_t>::format(sink, ip.b, num_spec);
      sink.push('.');
      formatter<uint8_t>::format(sink, ip.c, num_spec);
      sink.push('.');
      formatter<uint8_t>::format(sink, ip.d, num_spec);
    }
  };
}
```

== Obeying the `FormatSpec`
If the user provides specifiers like `{:>20}`, the custom formatter should obey them. The easiest way to do this is to format the custom struct into a local temporary buffer, then delegate to the `std::string_view` formatter, which handles all padding and alignment logic automatically.

```cpp
namespace klib {
  template<>
  struct formatter<CustomStruct> {
    template<typename Sink>
    static constexpr void format(Sink& sink, const CustomStruct& val, const FormatSpec& spec) noexcept {
      char temp[64];

      // Format to the temporary string
      std::size_t len = klib::kprint(temp, sizeof(temp), "VAL: v={:#x}", val.data);

      // Pass the resulting string and the user's spec to the string_view formatter to automatically handle
      // any user-requested padding.
      formatter<std::string_view>::format(buf, std::string_view(temp, len), spec);
    }
  };
}
```

= Architecture & Safety Guidelines
== The Compile-Time Trap
Since kernel enviroments are compiled with `-fno-exceptions`, we cannot rely on standard `throw std::runtime_error` when strings are malformed.

To enforce safety with exceptions, `kformat` uses the _Non-Constexpr Trap_. If the number of `{}` placeholders does not match the provided arguments, the `consteval` constructor intentionally calls an undefined, non-`constexpr` function:

```cpp
[[gnu::error("Format Argument mismatch")]] void compile_time_error_format_argument_mismatch();
```

This forces the compiler to instantly abort and print the error in the build log, guaranteeing that format mismatch bugs never reach the final binary.

== Fallback Truncation

When formatting to a bounded `StringBuffer`, `kformat` requests a direct memory block via `advance()`. If the buffer is nearing its capacity limit and cannot fulfill the request, the engine will gracefully fallback to a small local stack array, rendering the integer and pushing characters byte-by-byte until the exact buffer capacity is hit. This guarantees that `kformat` will never overwrite neighboring stack memory, even during kernel panic.
