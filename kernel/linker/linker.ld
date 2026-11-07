/*
 * Erebus kernel image layout.
 *
 * The kernel is a position-independent ELF linked for the top 2 GiB of the address space, as
 * -mcmodel=kernel requires.
 *
 *   requests  RW  Limine requests, which the bootloader answers in place
 *   text      RX  code, static call trampolines included
 *   rodata    R   constants, data that only holds addresses, and what Limine relocates with
 *   data      RW  initialized data and .bss
 *   init      R   tables only needed while booting
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* PF_X = 1, PF_W = 2, PF_R = 4 */
PHDRS {
    requests  PT_LOAD     FLAGS(4 | 2);
    text      PT_LOAD     FLAGS(4 | 1);
    rodata    PT_LOAD     FLAGS(4);
    data      PT_LOAD     FLAGS(4 | 2);
    init      PT_LOAD     FLAGS(4);
    dynamic   PT_DYNAMIC  FLAGS(4);
}

SECTIONS {
    . = 0xffffffff80000000;
    PROVIDE_HIDDEN(__image_base = .);

    /* Limine scans for its requests between the start and end markers. */
    .limine_requests : {
        PROVIDE_HIDDEN(__limine_requests_start = .);
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        PROVIDE_HIDDEN(__limine_requests_end = .);
    } :requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Cold code first and hot code next to each other, as GNU ld lays them out. */
    .text : {
        PROVIDE_HIDDEN(__text_start = .);
        *(.text.unlikely .text.unlikely.*)
        *(.text.hot .text.hot.*)
        *(.text .text.*)

        /* 16-byte slots that StaticCallPatcher rewrites. Nothing refers to the section itself. */
        . = ALIGN(16);
        PROVIDE_HIDDEN(__static_call_tramps_start = .);
        KEEP(*(static_call_tramps))
        PROVIDE_HIDDEN(__static_call_tramps_end = .);
        PROVIDE_HIDDEN(__text_end = .);
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        PROVIDE_HIDDEN(__rodata_start = .);
        *(SORT_BY_ALIGNMENT(.rodata .rodata.*))
    } :rodata

    /* Pointers and vtables. Limine relocates them before the kernel runs; read-only after. */
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.plt)
    } :rodata

    /* The test kernel's registered tests (tests/framework/ktest.hpp); empty in the real one. */
    .ktest : {
        . = ALIGN(8);
        PROVIDE_HIDDEN(__ktest_cases_start = .);
        KEEP(*(ktest_cases))
        PROVIDE_HIDDEN(__ktest_cases_end = .);
    } :rodata

    /* The relocations and the dynamic section Limine reads them through. */
    .dynamic   : { *(.dynamic) } :rodata :dynamic
    .rela.dyn  : { *(.rela.dyn) } :rodata
    .dynsym    : { *(.dynsym) } :rodata
    .dynstr    : { *(.dynstr) } :rodata
    .gnu.hash  : { *(.gnu.hash) } :rodata
    .hash      : { *(.hash) } :rodata
    PROVIDE_HIDDEN(__rodata_end = .);

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        PROVIDE_HIDDEN(__data_start = .);
        *(SORT_BY_ALIGNMENT(.data .data.*))
        PROVIDE_HIDDEN(__data_end = .);
    } :data

    .bss : {
        PROVIDE_HIDDEN(__bss_start = .);
        *(SORT_BY_ALIGNMENT(.bss .bss.*))
        *(COMMON)
        PROVIDE_HIDDEN(__bss_end = .);
    } :data

    /*
     * The kernel never runs global constructors or destructors. Whatever the compiler emits for
     * them lands here so that the assertion below refuses the link, instead of the object
     * silently staying unconstructed. Make such objects constinit, or build them in place.
     */
    .init_array : {
        KEEP(*(.preinit_array .init_array .init_array.* .ctors .ctors.*))
        KEEP(*(.fini_array .fini_array.* .dtors .dtors.*))
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Read while booting and never again, so the pages can be handed back later. */
    .init.rodata : {
        PROVIDE_HIDDEN(__init_start = .);

        /* What StaticCallPatcher picks the trampoline targets from. */
        . = ALIGN(8);
        PROVIDE_HIDDEN(__static_call_table_start = .);
        KEEP(*(static_call_table))
        PROVIDE_HIDDEN(__static_call_table_end = .);

        . = ALIGN(CONSTANT(MAXPAGESIZE));
        PROVIDE_HIDDEN(__init_end = .);
    } :init

    PROVIDE_HIDDEN(__image_end = .);

    /DISCARD/ : {
        /* Includes the empty functions that carry the static call trampolines. */
        *(.discard .discard.*)
        /* Built without exceptions, and the kernel has nothing that unwinds. */
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.interp)
    }
}

ASSERT(SIZEOF(.init_array) == 0, "a global object needs a constructor or destructor run; make it constinit or build it in place")
