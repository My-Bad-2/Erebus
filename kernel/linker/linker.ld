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
        /* Kernel virtual base */
        . = 0xffffffff80000000;
        PROVIDE(__image_base = .);
    
        .limine_requests : {
            PROVIDE_HIDDEN(__limine_requests_start = .);
            KEEP(*(.limine_requests_start))
            KEEP(*(.limine_requests))
            KEEP(*(.limine_requests_end))
            PROVIDE_HIDDEN(__limine_requests_end = .);
        } :requests
    
        /* Executable Code (RX) */
        . = ALIGN(CONSTANT(MAXPAGESIZE));
    
        .text : {
            PROVIDE_HIDDEN(__text_start = .);
            *(.text.hot .text.hot.*)
            *(.text .text.*)
            *(.text.unlikely .text.unlikely.*)
            PROVIDE_HIDDEN(__text_end = .);
        } :text
    
        /* Read-Only Data & Dynamic Relocations (R) */
        . = ALIGN(CONSTANT(MAXPAGESIZE));
    
        .rodata : {
            PROVIDE_HIDDEN(__rodata_start = .);
            *(SORT_BY_ALIGNMENT(.rodata .rodata.*))
            *(SORT_BY_ALIGNMENT(.srodata .srodata.*))
    
            /* Exception Fixup Table */
            . = ALIGN(8);
            PROVIDE_HIDDEN(__start_extable = .);
            KEEP(*(.extable))
            PROVIDE_HIDDEN(__stop_extable = .);
            PROVIDE_HIDDEN(__rodata_end = .);
        } :rodata
    
        . = ALIGN(8);
        .dynamic : {
            PROVIDE_HIDDEN(__dynamic_start = .);
            KEEP(*(.dynamic))
            PROVIDE_HIDDEN(__dynamic_end = .);
        } :rodata :dynamic
    
        .rela.dyn : {
            PROVIDE_HIDDEN(__rela_dyn_start = .);
            KEEP(*(.rela.dyn .rela.*))
            PROVIDE_HIDDEN(__rela_dyn_end = .);
        } :rodata
    
        .rel.dyn : {
            PROVIDE_HIDDEN(__rel_dyn_start = .);
            KEEP(*(.rel.dyn .rel.*))
            PROVIDE_HIDDEN(__rel_dyn_end = .);
        } :rodata
    
        .data.rel.ro : {
            PROVIDE_HIDDEN(__data_rel_ro_start = .);
            *(.data.rel.ro .data.rel.ro.*)
            PROVIDE_HIDDEN(__data_rel_ro_end = .);
        } :rodata
    
        /* Read-Write Data & BSS (RW) */
        . = ALIGN(CONSTANT(MAXPAGESIZE));
    
        .data : {
            PROVIDE_HIDDEN(__data_start = .);
            *(SORT_BY_ALIGNMENT(.data .data.*))
            *(SORT_BY_ALIGNMENT(.sdata .sdata.*))
            *(.got .got.*)
            *(.got.plt)
            PROVIDE_HIDDEN(__data_end = .);
        } :data
    
        . = ALIGN(CONSTANT(MAXPAGESIZE));
    
        .bss : {
            PROVIDE_HIDDEN(__bss_start = .);
            *(SORT_BY_ALIGNMENT(.bss .bss.*))
            *(SORT_BY_ALIGNMENT(.sbss .sbss.*))
            *(COMMON)
            . = ALIGN(8);
            PROVIDE_HIDDEN(__bss_end = .);
        } :data
    
        /* Reclaimable Boot/Init Section (RWX) */
        . = ALIGN(CONSTANT(MAXPAGESIZE));
    
        .init : {
            PROVIDE_HIDDEN(__init_start = .);
    
            . = ALIGN(4);
            PROVIDE_HIDDEN(__start_altinstructions = .);
            KEEP(*(.init.altinstructions))
            PROVIDE_HIDDEN(__stop_altinstructions = .);
    
            . = ALIGN(8);
            PROVIDE_HIDDEN(__start_static_calls = .);
            KEEP(*(.init.static_calls))
            PROVIDE_HIDDEN(__stop_static_calls = .);
    
            . = ALIGN(4);
            PROVIDE_HIDDEN(__start_static_keys = .);
            KEEP(*(.init.static_keys))
            PROVIDE_HIDDEN(__stop_static_keys = .);
    
            . = ALIGN(16);
            KEEP(*(.init.altinstr_replacement))
    
            . = ALIGN(16);
            *(.init.text .init.text.*)
            *(.init)
            *(.fini .fini.*)
    
            /* C++ Global Constructors */
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
    
            . = ALIGN(CONSTANT(COMMONPAGESIZE));
            PROVIDE_HIDDEN(__init_end = .);
        } :init
    
        PROVIDE_HIDDEN(__image_end = .);
    
        /* Drop non-loadable metadata and compile-time patcher anchors */
        /DISCARD/ : {
            *(.discard .discard.*)
            *(.note .note.*)
            *(.comment)
            *(.eh_frame*)
            *(.interp)
        }
}
