; where everything sits while the machine is starting.
;
; the assembly and the C cannot share a header, so this is the assembly
; copy of what boot/philemon.h says. tools/mkboot.py refuses to build if
; the two ever disagree, because a number that drifts in one of them is a
; machine that hangs with no explanation at all.

PH_MAGIC_LO   equ 0x4c4d4e30          ; "PHLMN0", split in half, because
PH_MAGIC_HI   equ 0x00005048          ; real mode writes 32 bits at a time
PH_TABLE_LBA  equ 32

TABLE_LINEAR    equ 0x00000500        ; the disk layout table
EARLY_LINEAR    equ 0x00000600        ; what the 64-bit half is told
MEMMAP_LINEAR   equ 0x00001000

BOUNCE_SEG      equ 0x2000            ; the bios reads into here, because
BOUNCE_OFF      equ 0x0000            ; it cannot write above a megabyte
BOUNCE_LINEAR   equ 0x00020000
BOUNCE_SECTORS  equ 64                ; 32 KiB at a time

HANDOFF64_LINEAR equ 0x00030000       ; the 64-bit half of me

VBE_SEG         equ 0x6000
VBE_INFO_OFF    equ 0x0000
VBE_MODE_OFF    equ 0x0400

E820_SEG        equ 0x7000
E820_OFF        equ 0x0000
E820_LINEAR     equ 0x00070000

KERNEL_LINEAR   equ 0x00800000        ; the kernel elf, still a file
RAMDISK_LINEAR  equ 0x01000000        ; the tar, where it stays
KERNEL_PHYS     equ 0x02000000        ; where the kernel ends up

PT_BASE         equ 0x00200000
PT_PML4         equ 0x00200000
PT_PDPT_LOW     equ 0x00201000
PT_PDPT_HIGH    equ 0x00202000
PT_PDPT_HHDM    equ 0x00203000
PT_PD0          equ 0x00204000        ; four of these, one per gigabyte
PT_PD_KERNEL    equ 0x00208000
PT_TOTAL_BYTES  equ 0x9000
