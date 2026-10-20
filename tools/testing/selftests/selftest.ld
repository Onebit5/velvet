/*
 * tools/testing/selftests/selftest.ld
 * where a selftest lives: down in the low half, where a program lives,
 * at 0x400000, where a static x86_64 program is linked. no higher half and
 * no -mcmodel=kernel, because a selftest is a program like any other
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : { *(.text .text.*) }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
