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
    __image_base = .;

    .limine_requests : {
        __limine_requests_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        __limine_requests_end = .;
    } :requests

    /* Align to page boundary. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        __text_start = .;

        *(.text.hot .text.hot.*)
        *(.text.unlikely .text.unlikely.*)
        . = ALIGN(64);
        *(.altinstr_replacement)

        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(SORT_BY_ALIGNMENT(.rodata .rodata.*))
        *(SORT_BY_ALIGNMENT(.srodata .srodata.*))

        /* Exception Fixup Table */
        . = ALIGN(8);
        __start_extable = .;
        *(.extable)
        __stop_extable = .;

        __rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(COMMONPAGESIZE));

    .dynamic : {
        __dynamic_start = .;
        KEEP(*(.dynamic))
        __dynamic_end = .;
    } :rodata :dynamic

    .rela.dyn : {
        __rela_dyn_start = .;
        KEEP(*(.rela.dyn .rela.*))
        __rela_dyn_end = .;
    } :rodata

    .rel.dyn : {
        __rel_dyn_start = .;
        KEEP(*(.rel.dyn .rel.*))
        __rel_dyn_end = .;
    } :rodata

    .data.rel.ro : {
        __data_rel_ro_start = .;
        *(.data.rel.ro .data.rel.ro.*)
        __data_rel_ro_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        __data_start = .;
        *(SORT_BY_ALIGNMENT(.data .data.*))
        *(SORT_BY_ALIGNMENT(.sdata .sdata.*))
        *(.got .got.*)
        *(.got.plt)
        __data_end = .;
    } :data

    .bss : ALIGN(CONSTANT(MAXPAGESIZE)) {
        . = ALIGN(64);
        __bss_start = .;
        *(SORT_BY_ALIGNMENT(.bss .bss.*))
        *(SORT_BY_ALIGNMENT(.sbss .sbss.*))
        *(COMMON)
        __bss_end = .;
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .init : {
        __init_start = .;

        /* Boot Code */
        *(.init.text .init.text.*)
        *(.init .init.*)
        *(.fini .fini.*)

        /* Static Keys & Alternative Instructions Metadata */
        . = ALIGN(8);
        __start_altinstructions = .;
        *(.init.altinstructions)
        __stop_altinstructions = .;

        . = ALIGN(8);
        __start_static_keys = .;
        *(.init.static_keys)
        __stop_static_keys = .;

            /* C++ Constructors */
        . = ALIGN(8);
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;

        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        __init_array_end = .;

        /* Ensure the init block end is page-aligned */
        . = ALIGN(CONSTANT(COMMONPAGESIZE));
        __init_end = .;
    } :init

    __image_end = .;

    /* Drop non-loadable metadata. */
    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame*)
        *(.interp)
    }
}
