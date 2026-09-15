/* Tell the linker that we want an x86_64 ELF64 output file */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

PHDRS {
    requests  PT_LOAD       FLAGS((1 << 1) | (1 << 2));             /* RW */
    text      PT_LOAD       FLAGS((1 << 0) | (1 << 2));             /* RX */
    rodata    PT_LOAD       FLAGS((1 << 2));                        /* R */
    data      PT_LOAD       FLAGS((1 << 1) | (1 << 2));             /* RW */
    dynamic   PT_DYNAMIC    FLAGS((1 << 2));                        /* R */
    init      PT_LOAD       FLAGS((1 << 0) | (1 << 1) | (1 << 2));  /* RWX */
}

SECTIONS {
    /* Kernel virtual base. */
    . = 0xffffffff80000000;
    PROVIDE(__image_base = .);

    PROVIDE_HIDDEN(__limine_requests_start = .);
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests
    PROVIDE_HIDDEN(__limine_requests_end = .);


    /* Align to page boundary. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    PROVIDE_HIDDEN(__text_start = .);
    .text : {
        *(.text .text.*)
        *(.text.hot .text.hot.*)
        *(.text.unlikely .text.unlikely.*)
        . = ALIGN(64);
        *(.altinstr_replacement)
    } :text
    PROVIDE_HIDDEN(__text_end = .);

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    PROVIDE_HIDDEN(__rodata_start = .);
    .rodata : {
        *(SORT_BY_ALIGNMENT(.rodata .rodata.*))
        *(SORT_BY_ALIGNMENT(.srodata .srodata.*))

        /* Exception Fixup Table */
        . = ALIGN(8);
        PROVIDE_HIDDEN(__start_extable = .);
        *(.extable)
        PROVIDE_HIDDEN(__stop_extable = .);
    } :rodata
    PROVIDE_HIDDEN(__rodata_end = .);

    . = ALIGN(CONSTANT(COMMONPAGESIZE));

    PROVIDE_HIDDEN(__dynamic_start = .);
    .dynamic : {
        KEEP(*(.dynamic))
    } :rodata :dynamic
    PROVIDE_HIDDEN(__dynamic_end = .);

    PROVIDE_HIDDEN(__rela_dyn_start = .);
    .rela.dyn : {
        KEEP(*(.rela.dyn .rela.*))
    } :rodata
    PROVIDE_HIDDEN(__rela_dyn_end = .);

    PROVIDE_HIDDEN(__rel_dyn_start = .);
    .rel.dyn : {
        KEEP(*(.rel.dyn .rel.*))
    } :rodata
    PROVIDE_HIDDEN(__rel_dyn_end = .);

    PROVIDE_HIDDEN(__data_rel_ro_start = .);
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } :rodata
    PROVIDE_HIDDEN(__data_rel_ro_end = .);

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    PROVIDE_HIDDEN(__data_start = .);
    .data : {
        *(SORT_BY_ALIGNMENT(.data .data.*))
        *(SORT_BY_ALIGNMENT(.sdata .sdata.*))
        *(.got .got.*)
        *(.got.plt)
    } :data
    PROVIDE_HIDDEN(__data_end = .);

    . = ALIGN(64);

    PROVIDE_HIDDEN(__bss_start = .);
    .bss : ALIGN(CONSTANT(MAXPAGESIZE)) {
        *(SORT_BY_ALIGNMENT(.bss .bss.*))
        *(SORT_BY_ALIGNMENT(.sbss .sbss.*))
        *(COMMON)
    } :data
    PROVIDE_HIDDEN(__bss_end = .);

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    PROVIDE_HIDDEN(__init_start = .);
    .init : {
        /* Boot Code */
        *(.init.text .init.text.*)
        *(.init .init.*)
        *(.fini .fini.*)

        /* Static Keys & Alternative Instructions Metadata */
        . = ALIGN(8);
        PROVIDE_HIDDEN(__start_altinstructions = .);
        *(.init.altinstructions)
        PROVIDE_HIDDEN(__stop_altinstructions = .);

        . = ALIGN(8);
        PROVIDE_HIDDEN(__start_static_keys = .);
        *(.init.static_keys)
        PROVIDE_HIDDEN(__stop_static_keys = .);

        /* C++ Constructors */
        . = ALIGN(8);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);

        . = ALIGN(8);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE_HIDDEN(__init_array_end = .);

        /* Ensure the init block end is page-aligned */
        . = ALIGN(CONSTANT(COMMONPAGESIZE));
    } :init
    PROVIDE_HIDDEN(__init_end = .);

    PROVIDE_HIDDEN(__image_end = .);

    /* Drop non-loadable metadata. */
    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame*)
        *(.interp)
    }
}
