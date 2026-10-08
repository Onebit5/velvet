#!/usr/bin/env python3
"""build a fat32 image and fill it from a directory.

there is no mkfs.vfat here, so this writes the filesystem by hand. that
is not the disaster it sounds like, fat32 is a boot sector describing
the layout, two copies of a table where entry N says which cluster
follows cluster N, and directories that are just files full of 32-byte
records.

the risk in writing both the formatter and the reader is that they
agree with each other and are both wrong. so this aims at the spec
rather than at my parser, and TESTING.md says to mount the result on
linux, if a driver nobody here wrote can read it, the layout is right.

usage: mkfat.py <output.img> <source-dir> [size-in-mib]
"""

import os
import struct
import sys

SECTOR = 512
RESERVED = 32           # boot sector, fsinfo, backup, and room to spare
NUM_FATS = 2
SEC_PER_CLUSTER = 1     # 512-byte clusters, so a small image still has
                        # the 65525 clusters fat32 is required to have

ATTR_READ_ONLY = 0x01
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE   = 0x20
ATTR_LFN       = 0x0f

EOC = 0x0fffffff


def not_bootable_stub(message):
    """16-bit code for the boot sector, at offset 0x5a where the jump at
    the very start lands. prints a line and halts, so a bios that tries
    to boot this disk says why instead of freezing."""
    code = bytes([
        0x31, 0xc0,              # xor ax, ax
        0x8e, 0xd8,              # mov ds, ax
        0x8e, 0xc0,              # mov es, ax
        0xbe, 0x00, 0x00,        # mov si, <patched below>
        0xac,                    # lodsb          <- loop
        0x84, 0xc0,              # test al, al
        0x74, 0x06,              # jz halt
        0xb4, 0x0e,              # mov ah, 0x0e   (teletype output)
        0xcd, 0x10,              # int 0x10
        0xeb, 0xf5,              # jmp loop
        0xf4,                    # hlt            <- halt
        0xeb, 0xfd,              # jmp halt
    ])
    # the bios loads this sector at 0000:7c00, so the message's address
    # is that plus wherever it ends up in the sector
    msg_at = 0x7c00 + 0x5a + len(code)
    code = code[:7] + struct.pack('<H', msg_at) + code[9:]
    return code + message.encode('ascii') + b'\r\n\0'


NOT_BOOTABLE = not_bootable_stub(
    'velvetOS: this disk holds files, not a system. boot the cd.')


def layout(total_sectors):
    """work out how big each fat has to be.

    circular: the fat has one entry per cluster, and how many clusters
    there are depends on how much room the fats take.

    the obvious fix, guess, recompute, repeat, does not converge. it
    oscillates between two sizes that each imply the other, and settles
    on whichever the loop happened to stop on. that leaves a filesystem
    claiming more clusters than its table has entries for, and the last
    couple of clusters have nowhere to record their chain: allocate one
    and the write lands in the second copy of the table instead.

    so grow instead of guessing. more table means fewer clusters, so the
    condition only ever goes from false to true, and the first size that
    satisfies it is the right one."""
    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED - NUM_FATS * fat_sectors
        clusters = data_sectors // SEC_PER_CLUSTER
        if clusters <= 0:
            raise RuntimeError('image too small to hold a filesystem')
        entries = fat_sectors * SECTOR // 4
        if entries >= clusters + 2:
            return fat_sectors, clusters
        fat_sectors += 1


def short_name(name, taken):
    """the 11-byte 8.3 name, which every entry has even when it also has
    a long one. names that do not fit get the ~1 treatment."""
    name = name.upper()
    base, dot, ext = name.rpartition('.')
    if not dot:
        base, ext = name, ''

    ok = lambda s: all(c.isalnum() or c in "$%'-_@~`!(){}^#&" for c in s)

    if len(base) <= 8 and len(ext) <= 3 and ok(base) and ok(ext):
        candidate = base.ljust(8) + ext.ljust(3)
        if candidate not in taken:
            taken.add(candidate)
            return candidate.encode('ascii'), False

    stem = ''.join(c for c in base if ok(c))[:6] or 'FILE'
    for n in range(1, 1000):
        suffix = '~%d' % n
        candidate = (stem[:8 - len(suffix)] + suffix).ljust(8) + ext[:3].ljust(3)
        if candidate not in taken:
            taken.add(candidate)
            return candidate.encode('ascii'), True
    raise RuntimeError('too many similar names: %s' % name)


def lfn_checksum(short):
    total = 0
    for byte in short:
        total = ((((total & 1) << 7) | (total >> 1)) + byte) & 0xff
    return total


def dir_entry(short, attr, cluster, size):
    return struct.pack('<11sBBBHHHHHHHI',
                       short, attr, 0, 0,
                       0, 0,                    # creation time/date
                       0,                       # last access
                       (cluster >> 16) & 0xffff,
                       0, 0,                    # write time/date
                       cluster & 0xffff,
                       size)


def lfn_entries(long_name, short):
    """the long name, in 13-character pieces, stored backwards in front
    of the short entry. the checksum ties them to it: change the short
    name and every long entry becomes garbage, which is deliberate,
    that is how an old dos tool deleting a file cannot leave the long
    half behind."""
    checksum = lfn_checksum(short)
    chars = [ord(c) for c in long_name]

    # pad: one null terminator, then 0xffff to the end of the last piece
    chars.append(0)
    while len(chars) % 13:
        chars.append(0xffff)

    pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    for index, piece in enumerate(pieces, start=1):
        sequence = index | (0x40 if index == len(pieces) else 0)
        entry = struct.pack('<B', sequence)
        entry += b''.join(struct.pack('<H', c) for c in piece[0:5])
        entry += struct.pack('<BBB', ATTR_LFN, 0, checksum)
        entry += b''.join(struct.pack('<H', c) for c in piece[5:11])
        entry += struct.pack('<H', 0)
        entry += b''.join(struct.pack('<H', c) for c in piece[11:13])
        assert len(entry) == 32
        out.append(entry)
    return b''.join(reversed(out))       # highest sequence number first


def needs_lfn(name, short):
    """a long entry is needed whenever the name is not exactly what the
    short one renders back to, which includes any lowercase at all,
    since 8.3 names have no case to speak of"""
    base = short[:8].rstrip(b' ').decode('ascii')
    ext = short[8:].rstrip(b' ').decode('ascii')
    return name != (base + '.' + ext if ext else base)


class Image:
    def __init__(self, total_sectors):
        self.total_sectors = total_sectors
        self.fat_sectors, self.clusters = layout(total_sectors)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0ffffff8
        self.fat[1] = 0x0fffffff
        self.data = {}                   # cluster -> bytes
        self.next_free = 3               # 2 is the root directory

    def alloc_chain(self, payload):
        """write bytes into as many clusters as they need, chained"""
        size = SEC_PER_CLUSTER * SECTOR
        chunks = [payload[i:i + size] for i in range(0, len(payload), size)]
        if not chunks:
            return 0                     # an empty file owns no clusters
        first = self.next_free
        for n, chunk in enumerate(chunks):
            cluster = self.next_free
            self.next_free += 1
            if self.next_free > self.clusters + 1:
                raise RuntimeError('image is full')
            self.data[cluster] = chunk.ljust(size, b'\0')
            self.fat[cluster] = EOC if n == len(chunks) - 1 else cluster + 1
        return first

    def build_dir(self, entries, cluster, parent_cluster, is_root):
        """entries is a list of (name, attr, first_cluster, size)"""
        blob = b''
        if not is_root:
            blob += dir_entry(b'.          ', ATTR_DIRECTORY, cluster, 0)
            blob += dir_entry(b'..         ', ATTR_DIRECTORY,
                              0 if parent_cluster == 2 else parent_cluster, 0)

        taken = set()
        for name, attr, first, size in entries:
            short, forced = short_name(name, taken)
            if forced or needs_lfn(name, short):
                blob += lfn_entries(name, short)
            blob += dir_entry(short, attr, first, size)
        return blob

    def place_dir(self, blob, cluster):
        size = SEC_PER_CLUSTER * SECTOR
        chunks = [blob[i:i + size] for i in range(0, len(blob), size)] or [b'']
        current = cluster
        for n, chunk in enumerate(chunks):
            self.data[current] = chunk.ljust(size, b'\0')
            if n == len(chunks) - 1:
                self.fat[current] = EOC
            else:
                nxt = self.next_free
                self.next_free += 1
                self.fat[current] = nxt
                current = nxt

    def write(self, path, label='VELVETOS'):
        boot = bytearray(SECTOR)
        boot[0:3] = b'\xeb\x58\x90'
        boot[3:11] = b'VELVETOS'
        struct.pack_into('<HBHBHHBHHHII', boot, 11,
                         SECTOR, SEC_PER_CLUSTER, RESERVED, NUM_FATS,
                         0,                      # root entries (fat32: 0)
                         0,                      # total sectors 16
                         0xf8,                   # media descriptor
                         0,                      # fat size 16 (fat32: 0)
                         63, 255,                # sectors/track, heads
                         0,                      # hidden sectors
                         self.total_sectors)
        struct.pack_into('<IHHI', boot, 36, self.fat_sectors, 0, 0, 2)
        struct.pack_into('<HH', boot, 48, 1, 6)  # fsinfo, backup boot
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into('<I', boot, 67, 0x54494e59)   # volume id, 'TINY'
        boot[71:82] = label.ljust(11).encode('ascii')
        boot[82:90] = b'FAT32   '
        # a filesystem has to end its boot sector with 0x55aa, and a bios
        # takes that to mean "bootable" and jumps to the code after the
        # bpb. leaving that code as zeroes means the machine wanders off
        # into nothing and hangs, which is exactly what it did. so put
        # something there that says what happened, the way every real
        # formatter has since dos.
        boot[0x5a:0x5a + len(NOT_BOOTABLE)] = NOT_BOOTABLE
        boot[510:512] = b'\x55\xaa'

        fsinfo = bytearray(SECTOR)
        fsinfo[0:4] = b'RRaA'
        fsinfo[484:488] = b'rrAa'
        struct.pack_into('<II', fsinfo, 488,
                         self.clusters - (self.next_free - 2),
                         self.next_free)
        fsinfo[510:512] = b'\x55\xaa'

        fat = bytearray()
        for entry in self.fat:
            fat += struct.pack('<I', entry)
        fat = fat.ljust(self.fat_sectors * SECTOR, b'\0')

        with open(path, 'wb') as f:
            f.write(boot)
            f.write(fsinfo)
            f.write(b'\0' * (SECTOR * 4))
            f.write(boot)                        # the backup, at sector 6
            f.write(b'\0' * (SECTOR * (RESERVED - 7)))
            for _ in range(NUM_FATS):
                f.write(fat)

            data_start = f.tell()
            f.truncate(self.total_sectors * SECTOR)
            for cluster, payload in sorted(self.data.items()):
                f.seek(data_start + (cluster - 2) * SEC_PER_CLUSTER * SECTOR)
                f.write(payload)
            f.seek(self.total_sectors * SECTOR - 1)
            f.write(b'\0')


def build(out_path, src_dir, size_mib):
    total_sectors = (size_mib * 1024 * 1024) // SECTOR
    img = Image(total_sectors)

    def walk(directory, cluster, parent_cluster, is_root):
        entries = []
        pending_dirs = []
        for name in sorted(os.listdir(directory)):
            full = os.path.join(directory, name)
            if os.path.isdir(full):
                child = img.next_free
                img.next_free += 1
                img.fat[child] = EOC
                pending_dirs.append((full, child, name))
                entries.append((name, ATTR_DIRECTORY, child, 0))
            else:
                with open(full, 'rb') as f:
                    payload = f.read()
                first = img.alloc_chain(payload)
                entries.append((name, ATTR_ARCHIVE, first, len(payload)))

        blob = img.build_dir(entries, cluster, parent_cluster, is_root)
        img.place_dir(blob, cluster)

        for full, child, _name in pending_dirs:
            walk(full, child, cluster, False)

    walk(src_dir, 2, 0, True)
    img.write(out_path)

    used = img.next_free - 2
    print('%s: %d MiB, %d clusters of %dB, %d used, %d sectors per fat'
          % (out_path, size_mib, img.clusters, SEC_PER_CLUSTER * SECTOR,
             used, img.fat_sectors))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    build(sys.argv[1], sys.argv[2], size)
