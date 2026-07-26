/* Tell the linker that we want an x86_64 ELF64 output file */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

PHDRS
{
    requests  PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* Write + Read */
    text      PT_LOAD FLAGS((1 << 0) | (1 << 2)); /* Execute + Read */
    rodata    PT_LOAD FLAGS((1 << 2));            /* Read only */
    dynamic   PT_DYNAMIC FLAGS((1 << 2));         /* Read only */
    data      PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* Write + Read */
}

SECTIONS
{
    /* Kernel virtual base. */
    . = 0xffffffff80000000;
    __image_base = .;

    .limine_requests : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __limine_requests_start = .;

        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))

        __limine_requests_end = .;
    } :requests

    /* Align to page boundary. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __text_start = .;
        fred_entry_page = .;
        KEEP(*(.text.fred_entry))
        KEEP(*(.text.idt_entry))

        *(.text .text.*)
        *(.init .init.*)
        *(.fini .fini.*)
        __text_end = .;
    } :text

    . = ALIGN(8);
    .altinstructions : {
        __alt_instructions_start = .;
        KEEP(*(.altinstructions))
        __alt_instructions_end = .;
    }

    .altinstr_replacement : {
        __altinstr_replacement_start = .;
        KEEP(*(.altinstr_replacement))
        __altinstr_replacement_end = .;
    }

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.gcc_except_table .gcc_except_table.*)

        . = ALIGN(8);
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;

        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        /* Map legacy ctors directly into the init_array */
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        __init_array_end = .;

        . = ALIGN(8);
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        /* Map legacy dtors directly into the fini_array */
        KEEP(*(SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.dtors))
        __fini_array_end = .;

        __rodata_end = .;
    } :rodata

    /* Keep dynamic relocations for KASLR/PIE loading. */
    .dynamic : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __dynamic_start = .;
        KEEP(*(.dynamic))
        __dynamic_end = .;
    } :rodata :dynamic

    .rela.dyn : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __rela_dyn_start = .;
        KEEP(*(.rela.dyn))
        KEEP(*(.rela.*))
        __rela_dyn_end = .;
    } :rodata

    .rel.dyn : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __rel_dyn_start = .;
        KEEP(*(.rel.dyn))
        KEEP(*(.rel.*))
        __rel_dyn_end = .;
    } :rodata

    /* Align to page boundary. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : ALIGN(CONSTANT(MAXPAGESIZE)) {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.*)
        *(.got.plt)
        __data_end = .;
    } :data

    .bss : ALIGN(CONSTANT(MAXPAGESIZE)) {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    __image_end = .;

    /* Drop non-loadable metadata. */
    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame*)
    }
}