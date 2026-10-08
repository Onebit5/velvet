#!/usr/bin/env python3
"""wrap filesystem images in a partition table.

a disk is not a filesystem; it is a table saying where several of them
are. mkext2.py and mkfat.py make the filesystems, and this puts them
somewhere, which until 0.2.15 nothing did, because the kernel read
whichever drive answered first at sector zero.

both tables are here, because a machine has to read both:

  mbr, from 1983: four sixteen-byte entries at offset 446 of sector 0,
  with 32-bit start and length. that is where the two terabyte limit
  comes from.

  gpt: a header at sector 1, an array of entries after it, 64-bit
  addresses, names, and crc32 over both, so a corrupt table can be
  known to be corrupt rather than followed. a gpt disk still carries an
  mbr holding one entry of type 0xEE spanning everything, so that an
  old tool sees a full disk and declines to helpfully repartition it.

usage:
  mkdisk.py out.img mbr  fs1.img[:type] [fs2.img[:type] ...]
  mkdisk.py out.img gpt  fs1.img[:name] [fs2.img[:name] ...]
"""

import os
import struct
import sys

SECTOR = 512
ALIGN = 2048            # one mebibyte, which is what every tool has aligned
                        # to since disks stopped having real cylinders


def crc32(data):
    """the reflected crc32, written out rather than imported, this is a
    disk format and the point is to show the format"""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


MBR_TYPES = {
    "linux": 0x83,
    "fat32": 0x0C,
    "fat16": 0x06,
    "swap": 0x82,
    "efi": 0xEF,
}

GPT_TYPES = {
    # stored the way microsoft writes guids: first three fields
    # little-endian, last two big-endian
    "linux": bytes([0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4]),
    "efi":   bytes([0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b]),
    "swap":  bytes([0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
                    0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f]),
}


def a_guid(seed):
    """a stable, obviously-fake guid. real ones are random; these are
    derived from a number so that two runs of this tool over the same
    inputs produce the same image, which matters more here than being
    unpredictable"""
    return bytes(((seed * 37 + i * 11) & 0xFF) for i in range(16))


def lay_out(images):
    """where each filesystem goes. aligned to a mebibyte, in order"""
    at = ALIGN
    out = []
    for path, label in images:
        size = os.path.getsize(path)
        sectors = (size + SECTOR - 1) // SECTOR
        out.append((path, label, at, sectors))
        at += sectors
        at = (at + ALIGN - 1) // ALIGN * ALIGN
    return out, at


def write_payloads(f, placed):
    for path, _, first, _ in placed:
        with open(path, "rb") as src:
            f.seek(first * SECTOR)
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)


def build_mbr(out, images):
    placed, end = lay_out(images)
    if len(placed) > 4:
        print("mbr holds four partitions. the fifth would need an extended "
              "one, which this does not make and the kernel does not walk")
        return 1

    total = end + ALIGN
    with open(out, "wb") as f:
        f.truncate(total * SECTOR)
        write_payloads(f, placed)

        sector = bytearray(SECTOR)
        for i, (path, label, first, sectors) in enumerate(placed):
            kind = MBR_TYPES.get(label, 0x83)
            e = bytearray(16)
            e[0] = 0x80 if i == 0 else 0x00      # bootable, by convention
            # the chs fields are filled with the "too big for chs" value
            # every tool has used since disks outgrew it in the nineties
            e[1:4] = bytes([0xFE, 0xFF, 0xFF])
            e[4] = kind
            e[5:8] = bytes([0xFE, 0xFF, 0xFF])
            struct.pack_into("<I", e, 8, first)
            struct.pack_into("<I", e, 12, sectors)
            sector[446 + i * 16: 446 + (i + 1) * 16] = e

        sector[510] = 0x55
        sector[511] = 0xAA
        f.seek(0)
        f.write(sector)

    print(f"{out}: mbr, {len(placed)} partition(s), "
          f"{total * SECTOR // (1024 * 1024)} MiB")
    for path, label, first, sectors in placed:
        print(f"  {os.path.basename(path):<16} {label:<8} "
              f"lba {first}..{first + sectors - 1}")
    return 0


def build_gpt(out, images):
    placed, end = lay_out(images)

    entry_count = 128
    entry_size = 128
    entries_sectors = entry_count * entry_size // SECTOR      # 32

    # sector 0 protective mbr, 1 header, 2..33 entries, then the
    # payloads, and the whole thing mirrored at the end
    first_usable = 2 + entries_sectors
    if placed and placed[0][2] < first_usable:
        print("the first partition overlaps the table")
        return 1

    total = end + entries_sectors + 2 + ALIGN
    last_lba = total - 1
    backup_header_lba = last_lba
    backup_entries_lba = last_lba - entries_sectors

    entries = bytearray(entry_count * entry_size)
    for i, (path, label, first, sectors) in enumerate(placed):
        e = bytearray(entry_size)
        e[0:16] = GPT_TYPES.get(label, GPT_TYPES["linux"])
        e[16:32] = a_guid(i + 1)
        struct.pack_into("<Q", e, 32, first)
        struct.pack_into("<Q", e, 40, first + sectors - 1)
        struct.pack_into("<Q", e, 48, 0)
        name = os.path.basename(path).rsplit(".", 1)[0][:36]
        e[56:56 + len(name) * 2] = name.encode("utf-16-le")
        entries[i * entry_size:(i + 1) * entry_size] = e

    entries_crc = crc32(entries)

    def header(my_lba, alt_lba, entries_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)     # revision 1.0
        struct.pack_into("<I", h, 12, 92)            # header size
        struct.pack_into("<I", h, 16, 0)             # crc, filled below
        struct.pack_into("<I", h, 20, 0)
        struct.pack_into("<Q", h, 24, my_lba)
        struct.pack_into("<Q", h, 32, alt_lba)
        struct.pack_into("<Q", h, 40, first_usable)
        struct.pack_into("<Q", h, 48, backup_entries_lba - 1)
        h[56:72] = a_guid(99)
        struct.pack_into("<Q", h, 72, entries_lba)
        struct.pack_into("<I", h, 80, entry_count)
        struct.pack_into("<I", h, 84, entry_size)
        struct.pack_into("<I", h, 88, entries_crc)
        struct.pack_into("<I", h, 16, crc32(bytes(h)))
        return bytes(h)

    with open(out, "wb") as f:
        f.truncate(total * SECTOR)
        write_payloads(f, placed)

        # the protective mbr: one entry of type 0xEE covering everything,
        # so an old tool sees a full disk rather than an empty one
        sector = bytearray(SECTOR)
        e = bytearray(16)
        e[4] = 0xEE
        struct.pack_into("<I", e, 8, 1)
        struct.pack_into("<I", e, 12, min(total - 1, 0xFFFFFFFF))
        sector[446:462] = e
        sector[510] = 0x55
        sector[511] = 0xAA
        f.seek(0)
        f.write(sector)

        f.seek(1 * SECTOR)
        f.write(header(1, backup_header_lba, 2).ljust(SECTOR, b"\0"))
        f.seek(2 * SECTOR)
        f.write(entries)

        # and the mirror at the far end, which is the whole reason gpt
        # survives a bad sector at the front
        f.seek(backup_entries_lba * SECTOR)
        f.write(entries)
        f.seek(backup_header_lba * SECTOR)
        f.write(header(backup_header_lba, 1, backup_entries_lba)
                .ljust(SECTOR, b"\0"))

    print(f"{out}: gpt, {len(placed)} partition(s), "
          f"{total * SECTOR // (1024 * 1024)} MiB")
    for path, label, first, sectors in placed:
        print(f"  {os.path.basename(path):<16} {label:<8} "
              f"lba {first}..{first + sectors - 1}")
    return 0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1

    out, scheme = sys.argv[1], sys.argv[2]
    images = []
    for arg in sys.argv[3:]:
        if ":" in arg:
            path, label = arg.rsplit(":", 1)
        else:
            path, label = arg, "linux"
        if not os.path.exists(path):
            print(f"no such image: {path}")
            return 1
        images.append((path, label))

    if scheme == "mbr":
        return build_mbr(out, images)
    if scheme == "gpt":
        return build_gpt(out, images)

    print(f"unknown scheme {scheme}, mbr or gpt")
    return 1


if __name__ == "__main__":
    sys.exit(main())
