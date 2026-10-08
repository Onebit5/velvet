/*
 * philemon's 64-bit half: a flat binary at a fixed address, so the
 * 16-bit half can drop it there and jump without knowing what an elf is
 */
ENTRY(philemon_main)

SECTIONS
{
    . = 0x30000;

    /*
     * philemon_main has to be first: jumping to 0x30000 is the whole of
     * the calling convention between the two halves
     */
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
