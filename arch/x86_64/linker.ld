/*
 * linker script for the kernel.
 *
 * the __*_start/__*_end symbols exist so the vmm can map each section with
 * the permissions it deserves: text executable but not writable, rodata
 * neither, data writable but never executable. every section boundary is
 * page aligned because permissions come at page granularity.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* higher half, top 2GiB. this is what -mcmodel=kernel expects */
    . = 0xffffffff80000000;

    __kernel_start = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_end = .;
    __rodata_start = .;

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /*
     * the symbol table, generated from a first link pass and folded in
     * by a second. it sits after .text deliberately: growing it shifts
     * .data around but can never move a function, which is the whole
     * reason a two-pass build is sound rather than a fixed point we
     * have to iterate towards. read-only and non-executable like the
     * rest of rodata.
     */
    .ksyms : {
        *(.ksyms)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_end = .;
    __data_start = .;

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
