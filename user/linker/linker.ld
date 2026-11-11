/*
 * Erebus user-space Root Task (and static user binary) linker script.
 *
 * Virtual address layout:
 *   - [0x0000000000000000, 0x0000000000400000) : Unmapped 4 MiB null-pointer guard range
 *   - 0x0000000000400000                       : Image base (.text, .rodata, .data, .bss)
 *   - 0x0000004000000000 (USER_SPACE_BASE)     : Start of dynamic heap & mmap VMA regions
 *
 * Each PT_LOAD segment is aligned to 4 KiB (MAXPAGESIZE) so the kernel ELF loader can map
 * segments with strict W^X permissions:
 *   text    (PF_R | PF_X) : executable code
 *   rodata  (PF_R)        : read-only constants, jump tables, and build-id
 *   data    (PF_R | PF_W) : initialized writable data and zero-initialized .bss
 *   tls     (PF_R)        : optional Thread-Local Storage master template (.tdata + .tbss)
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* PF_X = 1, PF_W = 2, PF_R = 4. A static executable has no PT_DYNAMIC, and the kernel loader
 * takes no PT_LOAD that nothing is in. */
PHDRS {
    text      PT_LOAD     FLAGS(4 | 1);
    rodata    PT_LOAD     FLAGS(4);
    data      PT_LOAD     FLAGS(4 | 2);
    tls       PT_TLS      FLAGS(4);
}

SECTIONS {
    . = 0x0000000000400000; /* 4 MiB */
    PROVIDE_HIDDEN(__executable_start = .);
    PROVIDE_HIDDEN(__image_base = .);

    .text : ALIGN(CONSTANT(MAXPAGESIZE)) {
        PROVIDE_HIDDEN(__text_start = .);
        KEEP(*(.text._start))
        *(.text.unlikely .text.unlikely.*)
        *(.text.hot .text.hot.*)
        *(.text .text.*)
        PROVIDE_HIDDEN(__text_end = .);
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : ALIGN(CONSTANT(MAXPAGESIZE)) {
        PROVIDE_HIDDEN(__rodata_start = .);
        *(SORT_BY_ALIGNMENT(.rodata .rodata.*))
        *(.note.gnu.build-id)
    } :rodata

    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);

        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);

        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :rodata

    .data.rel.ro : ALIGN(8) {
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.plt)
    } :rodata

    PROVIDE_HIDDEN(__rodata_end = .);

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .tdata : ALIGN(16) {
        PROVIDE_HIDDEN(__tdata_start = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE_HIDDEN(__tdata_end = .);
    } :data :tls

    .tbss (NOLOAD) : ALIGN(16) {
        PROVIDE_HIDDEN(__tbss_start = .);
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        PROVIDE_HIDDEN(__tbss_end = .);
    } :tls

    .data : ALIGN(CONSTANT(MAXPAGESIZE)) {
        PROVIDE_HIDDEN(__data_start = .);
        *(SORT_BY_ALIGNMENT(.data .data.*))
        PROVIDE_HIDDEN(__data_end = .);
    } :data

    .bss : ALIGN(64) {
        PROVIDE_HIDDEN(__bss_start = .);
        *(SORT_BY_ALIGNMENT(.bss .bss.*))
        *(COMMON)
        . = ALIGN(16);
        PROVIDE_HIDDEN(__bss_end = .);
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__image_end = .);

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.interp)
        *(.dynamic .dynsym .dynstr .gnu.hash .hash)
    }
}