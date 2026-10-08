#!/usr/bin/env python3
"""build a bootable disk image for philemon.

there is no filesystem in the first 512 bytes, there is barely room for
anything in the first 512 bytes, so the pieces go at fixed places and a
table in a sector of its own says where. this writes that layout:

    lba 0..31    philemon, whose first sector is the one the bios reads
    lba 32       the table saying where everything else is
    lba 33..     philemon's 64-bit half, which is C
    then         the kernel, as an elf file
    then         the ramdisk tar
    then         the source tar, which nothing loads

it also checks that the assembly and the C agree about all of it, since
they cannot share a header and a number that drifts in one of them
produces a machine that hangs with no explanation.

the last of those is the odd one and 0.3.26 is why: the source tree is
written into the image and *left there*. philemon does not read it, the
kernel does not load it, and nothing about booting depends on it. it is
sectors with a tar in them, and the table is how anybody finds it again.

usage: mkboot.py <out.img> <philemon.bin> <philemon64.bin>
                 <kernel.elf> [ramdisk.tar [source.tar]]
"""

import os
import re
import struct
import sys

SECTOR = 512
LOADER_MAX_SECTORS = 32     # lba 0 through 31, then the table
BOOT_TABLE_LBA = 32
MAGIC = 0x50484c4d4e30       # "PHLMN0"

# the boot table, as boot/handoff.h declares it: nine 64-bit fields
TABLE_FIELDS = [
    'magic',
    'handoff_lba', 'handoff_sectors',
    'kernel_lba', 'kernel_sectors', 'kernel_size',
    'ramdisk_lba', 'ramdisk_sectors', 'ramdisk_size',
    'source_lba', 'source_sectors', 'source_size',
]


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


def check_agreement(root):
    """the assembly and the C describe the same memory, in two files that
    cannot include each other. a number that drifts in one of them is a
    machine that hangs, so this refuses to build rather than let it."""
    inc = open(os.path.join(root, 'boot/philemon.inc')).read()
    hdr = open(os.path.join(root, 'boot/philemon.h')).read()
    asm = open(os.path.join(root, 'boot/philemon.asm')).read()

    def from_inc(name):
        m = re.search(r'^%s\s+equ\s+(\S+)' % name, inc, re.M)
        return int(m.group(1), 0) if m else None

    def from_hdr(name):
        m = re.search(r'#define\s+%s\s+(0x[0-9a-fA-F]+)' % name, hdr)
        return int(m.group(1), 0) if m else None

    same = [
        ('TABLE_LINEAR',     'PH_TABLE_ADDR'),
        ('EARLY_LINEAR',     'PH_HANDOFF_ADDR'),
        ('MEMMAP_LINEAR',    'PH_MEMMAP_ADDR'),
        ('BOUNCE_LINEAR',    'PH_BOUNCE_ADDR'),
        ('HANDOFF64_LINEAR', 'PH_HANDOFF64'),
        ('VBE_SEG',          None),
        ('E820_LINEAR',      'PH_E820_ADDR'),
        ('PT_BASE',          'PH_PAGETABLE'),
        ('KERNEL_LINEAR',    'PH_KERNEL_IMAGE'),
        ('RAMDISK_LINEAR',   'PH_RAMDISK_ADDR'),
        ('KERNEL_PHYS',      'PH_KERNEL_PHYS'),
    ]
    for asm_name, c_name in same:
        if c_name is None:
            continue
        a, c = from_inc(asm_name), from_hdr(c_name)
        if a is None or c is None:
            sys.exit('mkboot: cannot find %s / %s to compare' % (asm_name, c_name))
        if a != c:
            sys.exit('mkboot: %s is %#x in the assembly but %s is %#x in the C'
                     % (asm_name, a, c_name, c))

    # the magic, which is what stops the loader believing a stray sector
    lo, hi = from_inc('PH_MAGIC_LO'), from_inc('PH_MAGIC_HI')
    if (hi << 32 | lo) != MAGIC:
        sys.exit('mkboot: the boot magic does not match between asm and this tool')

    # FIXME: this checks one hand written struct and not the other. the
    # assembly writes ph_early field by field the same way, and those
    # offsets are checked nowhere, which is how boot/philemon.asm came to
    # store boot_drive at 108 while struct ph_early puts the field at
    # 112. the check below is also keyed on one literal spelling, so it
    # covers the struct it happens to name, and it only asks that each
    # offset be a multiple of eight rather than the offset that field
    # belongs at, so a swapped pair passes. compare both structs against
    # the header, field by field.
    # and the boot table's field offsets, which stage 2 reads by hand
    used = sorted(set(int(n) for n in re.findall(r'TABLE_LINEAR \+ (\d+)', asm)))
    expected = [i * 8 for i in range(1, len(TABLE_FIELDS))]
    for off in used:
        if off not in expected:
            sys.exit('mkboot: stage2 reads the boot table at +%d, which is not '
                     'the start of any field. the two have drifted apart' % off)


def build(out, loader, loader64, kernel, ramdisk, source=None):
    ph = open(loader, 'rb').read()
    ho = open(loader64, 'rb').read()
    kr = open(kernel, 'rb').read()
    rd = open(ramdisk, 'rb').read() if ramdisk else b''
    sr = open(source, 'rb').read() if source else b''

    if len(ph) < SECTOR:
        sys.exit('mkboot: philemon is %d bytes, less than one sector'
                 % len(ph))
    if ph[510:512] != b'\x55\xaa':
        sys.exit('mkboot: philemon has no boot signature at 510; no bios '
                 'would run it')
    if sectors(len(ph)) > LOADER_MAX_SECTORS:
        sys.exit('mkboot: philemon is %d sectors and only %d fit before the '
                 'table' % (sectors(len(ph)), LOADER_MAX_SECTORS))

    handoff_lba = BOOT_TABLE_LBA + 1
    kernel_lba = handoff_lba + sectors(len(ho))
    ramdisk_lba = kernel_lba + sectors(len(kr))
    # the source goes last, and that is not arbitrary. everything before
    # it is read by the loader in the order it is written, and the
    # source is read by nobody at boot, so putting it at the end keeps
    # every offset that matters exactly where it was, and a machine with
    # no source tree in its image is the same image minus a tail
    source_lba = ramdisk_lba + sectors(len(rd))
    total = source_lba + sectors(len(sr))

    table = struct.pack('<12Q', MAGIC,
                        handoff_lba, sectors(len(ho)),
                        kernel_lba, sectors(len(kr)), len(kr),
                        ramdisk_lba, sectors(len(rd)), len(rd),
                        source_lba if sr else 0, sectors(len(sr)), len(sr))
    table = table.ljust(SECTOR, b'\0')

    # pad the image out to something a bios will accept as a hard disk.
    # geometry is still computed the 1981 way, 16 heads, 63 sectors per
    # track, and an image smaller than one cylinder of that comes out
    # as zero cylinders, which some firmware reads as "no drive here" and
    # refuses to boot. 8 MiB is comfortably past that and costs nothing
    # but zeroes
    CYLINDER = 16 * 63 * SECTOR
    floor = max(8 * 1024 * 1024, CYLINDER * 4)
    if total * SECTOR < floor:
        total = floor // SECTOR
    total = ((total * SECTOR + CYLINDER - 1) // CYLINDER) * CYLINDER // SECTOR

    image = bytearray(total * SECTOR)
    image[0:len(ph)] = ph
    image[BOOT_TABLE_LBA * SECTOR:(BOOT_TABLE_LBA + 1) * SECTOR] = table
    image[handoff_lba * SECTOR:handoff_lba * SECTOR + len(ho)] = ho
    image[kernel_lba * SECTOR:kernel_lba * SECTOR + len(kr)] = kr
    if rd:
        image[ramdisk_lba * SECTOR:ramdisk_lba * SECTOR + len(rd)] = rd
    if sr:
        image[source_lba * SECTOR:source_lba * SECTOR + len(sr)] = sr

    with open(out, 'wb') as f:
        f.write(image)

    print('%s: philemon %dB (%d sectors), 64-bit half %dB, kernel %dB, '
          'ramdisk %dB, source %dB -> %d sectors'
          % (out, len(ph), sectors(len(ph)), len(ho), len(kr), len(rd),
             len(sr), total))


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    check_agreement(os.path.dirname(here))
    build(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
          sys.argv[5] if len(sys.argv) > 5 else None,
          sys.argv[6] if len(sys.argv) > 6 else None)
