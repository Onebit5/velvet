#!/usr/bin/env python3
"""read an ext4 image, or an ext2 one, and complain about it.

this is deliberately a *second opinion*. there is no e2fsck on this
machine, so the formatter and the kernel driver would otherwise be two
programs by one author agreeing with each other, which is not a check,
it is an echo. that already happened once: mkfat.py and the fat32 parser
agreed on a cluster count that was wrong, and the only thing that found
it was a reader written separately from the layout.

so nothing here imports a formatter or shares a line with the driver.
it walks the image the way the on-disk format says to, and checks the
things a filesystem is allowed to assume about itself:

  every block an inode claims is marked used in a bitmap
  no block is claimed by two inodes
  the free counts in the superblock and the group descriptors are true
  every directory entry points at an inode that is in use
  every directory has . and .., and .. really is its parent
  link counts match the number of names actually pointing at each inode

0.3.22 made the driver ext4, so this reads both maps a file may have:
the fifteen block numbers ext2 kept, and the extent tree ext4 keeps in
the same sixty bytes. which one a file uses is a flag in its inode
rather than a property of the filesystem, an ext4 disk holds files of
both kinds, and a checker that assumed otherwise would read a header as
a block number and go looking a hundred thousand blocks away.

a disk stopped being a filesystem in 0.2.15, so this looks for a
partition table first and reads the filesystem inside whichever
partition has one. an image written straight to sector zero still works
exactly as it did, that is a partition table of no entries as far as
this is concerned.

usage:
  readext4.py <image>            check it, and say what is wrong
  readext4.py <image> --tree     also print the tree it found
  readext4.py <image> --cat P    print the contents of one path
  readext4.py <image> --part N   read partition N rather than guessing
"""

import struct
import sys

EXT2_MAGIC = 0xEF53
S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0xF000, 0x8000, 0x4000, 0xA000

INCOMPAT_FILETYPE = 0x0002
INCOMPAT_RECOVER = 0x0004
INCOMPAT_EXTENTS = 0x0040
INCOMPAT_64BIT = 0x0080

COMPAT_HAS_JOURNAL = 0x0004

# the log, which is the one structure on this disk that is big endian.
# it is a file, inode 8 by default, and the superblock says which,
# so finding it is the ordinary block map and nothing special
JBD_MAGIC = 0xC03B3998
JBD_SUPERBLOCK_V1, JBD_SUPERBLOCK_V2 = 3, 4

EXTENTS_FL = 0x00080000
EXTENT_MAGIC = 0xF30A


class Bad(Exception):
    pass


#
# written the same way as everything else here: from the on-disk layout
# rather than from tools/mkdisk.py, so that the two disagreeing is
# something this can notice

SECTOR = 512


def partitions(raw):
    """every partition the image says it has, as (first_byte, name)"""
    if len(raw) < SECTOR or raw[510] != 0x55 or raw[511] != 0xAA:
        return []

    out = []
    protective = False
    for i in range(4):
        e = raw[446 + i * 16: 446 + (i + 1) * 16]
        kind = e[4]
        first = struct.unpack_from("<I", e, 8)[0]
        count = struct.unpack_from("<I", e, 12)[0]
        if kind == 0xEE:
            protective = True
            break
        if kind == 0 or count == 0:
            continue
        out.append((first * SECTOR, f"mbr entry {i + 1}, type {kind:#04x}"))

    if not protective:
        return out

    # a gpt disk. the header is at sector 1 and the entries after it
    h = raw[SECTOR:SECTOR * 2]
    if h[0:8] != b"EFI PART":
        return []
    entries_lba = struct.unpack_from("<Q", h, 72)[0]
    entry_count = struct.unpack_from("<I", h, 80)[0]
    entry_size = struct.unpack_from("<I", h, 84)[0]

    out = []
    for i in range(min(entry_count, 128)):
        at = entries_lba * SECTOR + i * entry_size
        e = raw[at:at + entry_size]
        if len(e) < 56 or e[0:16] == bytes(16):
            continue
        first = struct.unpack_from("<Q", e, 32)[0]
        name = e[56:128].decode("utf-16-le", errors="replace").split("\0")[0]
        out.append((first * SECTOR, f"gpt entry {i + 1} \"{name}\""))
    return out


def find_ext2(raw, want):
    """the bytes of the filesystem, and how it was found"""
    parts = partitions(raw)

    if want is not None:
        if want >= len(parts):
            raise Bad(f"there is no partition {want}, found {len(parts)}")
        at, how = parts[want]
        return raw[at:], how

    if not parts:
        return raw, "the whole image"

    for at, how in parts:
        chunk = raw[at:]
        if len(chunk) > 2048 and struct.unpack_from("<H", chunk, 1024 + 56)[0] \
                == EXT2_MAGIC:
            return chunk, how

    raise Bad(f"{len(parts)} partition(s), none of them holding ext2")


class Ext2:
    def __init__(self, raw):
        self.raw = raw

        sb = raw[1024:2048]
        if struct.unpack_from("<H", sb, 56)[0] != EXT2_MAGIC:
            raise Bad("no ext2 magic where the superblock should be")

        self.inodes_count = struct.unpack_from("<I", sb, 0)[0]
        self.blocks_count = struct.unpack_from("<I", sb, 4)[0]
        self.free_blocks = struct.unpack_from("<I", sb, 12)[0]
        self.free_inodes = struct.unpack_from("<I", sb, 16)[0]
        self.first_data_block = struct.unpack_from("<I", sb, 20)[0]
        self.block_size = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.blocks_per_group = struct.unpack_from("<I", sb, 32)[0]
        self.inodes_per_group = struct.unpack_from("<I", sb, 40)[0]
        self.rev = struct.unpack_from("<I", sb, 76)[0]
        self.inode_size = struct.unpack_from("<H", sb, 88)[0] if self.rev else 128
        self.first_ino = struct.unpack_from("<I", sb, 84)[0] if self.rev else 11
        self.incompat = struct.unpack_from("<I", sb, 96)[0] if self.rev else 0
        self.compat = struct.unpack_from("<I", sb, 92)[0] if self.rev else 0
        self.journal_ino = (struct.unpack_from("<I", sb, 224)[0]
                            if self.rev else 0)
        self.label = sb[120:136].split(b"\0")[0].decode(errors="replace")

        self.groups = (self.blocks_count - self.first_data_block
                       + self.blocks_per_group - 1) // self.blocks_per_group

        # the descriptors doubled in size when 64-bit arrived, and the
        # size is in the superblock rather than implied, so it is read
        # rather than assumed. the second half holds the high halves of
        # the numbers in the first, all of which are zero on any disk
        # small enough to be described in 32 bits
        self.desc_size = 32
        if self.incompat & INCOMPAT_64BIT:
            self.desc_size = struct.unpack_from("<H", sb, 254)[0]
            if self.desc_size < 64:
                raise Bad(f"64-bit, with {self.desc_size}-byte descriptors")

        gdt_at = (self.first_data_block + 1) * self.block_size
        self.gd = []
        for g in range(self.groups):
            at = gdt_at + g * self.desc_size
            d = raw[at:at + self.desc_size]
            self.gd.append({
                "block_bitmap": struct.unpack_from("<I", d, 0)[0],
                "inode_bitmap": struct.unpack_from("<I", d, 4)[0],
                "inode_table": struct.unpack_from("<I", d, 8)[0],
                "free_blocks": struct.unpack_from("<H", d, 12)[0],
                "free_inodes": struct.unpack_from("<H", d, 14)[0],
                "used_dirs": struct.unpack_from("<H", d, 16)[0],
            })

        self.per_block = self.block_size // 4


    def block(self, n):
        at = n * self.block_size
        return self.raw[at:at + self.block_size]

    def inode(self, ino):
        g = (ino - 1) // self.inodes_per_group
        idx = (ino - 1) % self.inodes_per_group
        at = (self.gd[g]["inode_table"] * self.block_size
              + idx * self.inode_size)
        d = self.raw[at:at + self.inode_size]
        return {
            "mode": struct.unpack_from("<H", d, 0)[0],
            "uid": struct.unpack_from("<H", d, 2)[0],
            "size": struct.unpack_from("<I", d, 4)[0],
            "atime": struct.unpack_from("<I", d, 8)[0],
            "ctime": struct.unpack_from("<I", d, 12)[0],
            "mtime": struct.unpack_from("<I", d, 16)[0],
            "gid": struct.unpack_from("<H", d, 24)[0],
            "links": struct.unpack_from("<H", d, 26)[0],
            "blocks512": struct.unpack_from("<I", d, 28)[0],
            "flags": struct.unpack_from("<I", d, 32)[0],
            "ptrs": [struct.unpack_from("<I", d, 40 + i * 4)[0]
                     for i in range(15)],
            "raw": d,
        }

    def block_in_use(self, n):
        g = (n - self.first_data_block) // self.blocks_per_group
        i = (n - self.first_data_block) % self.blocks_per_group
        bm = self.block(self.gd[g]["block_bitmap"])
        return (bm[i // 8] >> (i % 8)) & 1

    def inode_in_use(self, ino):
        g = (ino - 1) // self.inodes_per_group
        i = (ino - 1) % self.inodes_per_group
        bm = self.block(self.gd[g]["inode_bitmap"])
        return (bm[i // 8] >> (i % 8)) & 1


    def is_fast_symlink(self, ino_data):
        """a symlink short enough to live *in* its own block pointers.

        this is the one place in ext2 where those fifteen words are not
        block numbers at all, and anything that walks them without
        asking first reads the target as a list of addresses, which is
        what happens if you forget: four enormous block numbers and a
        filesystem that looks corrupt"""
        return ((ino_data["mode"] & S_IFMT) == S_IFLNK
                and ino_data["size"] < 60)

    def has_extents(self, ino_data):
        return bool(ino_data["flags"] & EXTENTS_FL)

    def extent_blocks(self, ino_data, want_meta=False):
        """every data block of an extent-mapped inode, in order.

        an extent is a run: "the next N logical blocks are at physical
        P". walking one means expanding it back into the list ext2 would
        have stored, which is the wrong thing to do to a filesystem and
        the right thing to do to a checker, everything below wants to
        know which blocks are claimed, and by whom.

        holes are not skipped so much as absent: a logical block no
        extent covers is a hole, and reads as zeroes. so the list this
        returns is keyed by logical block, not dense."""
        out = []

        def walk(node, max_here, depth_seen):
            if struct.unpack_from("<H", node, 0)[0] != EXTENT_MAGIC:
                raise Bad("an extent node with no magic at the top of it")
            entries = struct.unpack_from("<H", node, 2)[0]
            maximum = struct.unpack_from("<H", node, 4)[0]
            depth = struct.unpack_from("<H", node, 6)[0]
            if entries > maximum or maximum > max_here:
                raise Bad(f"an extent node claiming {entries} of {maximum} "
                          f"records where only {max_here} fit")
            if depth_seen > 5:
                raise Bad("an extent tree deeper than ext4 can be")

            for i in range(entries):
                rec = node[12 + i * 12: 24 + i * 12]
                if depth == 0:
                    first = struct.unpack_from("<I", rec, 0)[0]
                    length = struct.unpack_from("<H", rec, 4)[0]
                    length = length - 32768 if length > 32768 else length
                    start = (struct.unpack_from("<H", rec, 6)[0] << 32) \
                        | struct.unpack_from("<I", rec, 8)[0]
                    for k in range(length):
                        out.append((first + k, start + k))
                else:
                    child = (struct.unpack_from("<H", rec, 8)[0] << 32) \
                        | struct.unpack_from("<I", rec, 4)[0]
                    if want_meta:
                        out.append((None, child))
                    walk(self.block(child), (self.block_size - 12) // 12,
                         depth_seen + 1)

        walk(ino_data["raw"][40:100], (60 - 12) // 12, 0)
        return out

    def blocks_of(self, ino_data, want_meta=False):
        """every data block of an inode, in order. `want_meta` also
        yields the indirect blocks themselves, which is what a check of
        the bitmap needs and what reading the file does not"""
        if self.is_fast_symlink(ino_data):
            return []

        if self.has_extents(ino_data):
            return [phys for _logical, phys
                    in self.extent_blocks(ino_data, want_meta)]

        out = []
        p = ino_data["ptrs"]

        for i in range(12):
            if p[i]:
                out.append(p[i])

        def indirect(b, depth):
            if not b:
                return
            if want_meta:
                out.append(b)
            table = self.block(b)
            for i in range(self.per_block):
                n = struct.unpack_from("<I", table, i * 4)[0]
                if not n:
                    continue
                if depth == 1:
                    out.append(n)
                else:
                    indirect(n, depth - 1)

        indirect(p[12], 1)
        indirect(p[13], 2)
        indirect(p[14], 3)
        return out

    def read_file(self, ino_data):
        if self.is_fast_symlink(ino_data):
            return ino_data["raw"][40:40 + ino_data["size"]]

        size = ino_data["size"]
        if self.has_extents(ino_data):
            # placed by logical block rather than concatenated, because
            # an extent map is allowed to have holes in it, and a hole
            # reads as zeroes, not as the next block along. joining the
            # blocks in order would shift the whole rest of the file
            out = bytearray(size)
            for logical, phys in self.extent_blocks(ino_data):
                if logical is None:
                    continue
                at = logical * self.block_size
                if at >= size:
                    continue
                chunk = self.block(phys)[:size - at]
                out[at:at + len(chunk)] = chunk
            return bytes(out)

        data = b"".join(self.block(b) for b in self.blocks_of(ino_data))
        return data[:size]

    def readdir(self, ino_data):
        out = []
        for b in self.blocks_of(ino_data):
            data = self.block(b)
            at = 0
            while at < self.block_size:
                ino, rec, nlen, ftype = struct.unpack_from("<IHBB", data, at)
                if rec < 8:
                    raise Bad(f"a directory entry claims to be {rec} bytes")
                if ino:
                    name = data[at + 8:at + 8 + nlen].decode(errors="replace")
                    out.append((name, ino, ftype))
                at += rec
        return out


def journal(fs):
    """the log, read from the format rather than from the driver.

    this exists for the same reason the rest of this file does: the
    kernel's journal and the kernel's replay agree with each other by
    construction, and a format nobody else has read is a format nobody
    has checked. so the log is walked here from its own specification,
    big endian, twelve bytes of header, a superblock in block zero,
    and the two things worth insisting on are said out loud.

    returns (description, problems). a filesystem with no journal is
    not a problem: that is 0.3.23's arrangement and works exactly as it
    did.
    """
    problems = []
    if not (fs.compat & COMPAT_HAS_JOURNAL):
        if fs.incompat & INCOMPAT_RECOVER:
            problems.append("the superblock asks to be recovered, and there "
                            "is no journal to recover it from")
        return None, problems

    ino = fs.journal_ino or 8
    if not fs.inode_in_use(ino):
        return None, [f"a journal is claimed, but inode {ino} is free"]

    d = fs.inode(ino)
    blocks = list(fs.blocks_of(d))
    if not blocks:
        return None, [f"a journal is claimed, but inode {ino} has no blocks"]

    # a log the allocator could hand out to somebody else is the worst
    # thing on this list, and it would look like nothing until the day
    # the journal was needed
    for b in blocks:
        if b >= fs.blocks_count:
            problems.append(f"the journal has block {b}, past the end of "
                            f"the image")
        elif not fs.block_in_use(b):
            problems.append(f"the journal has block {b}, which the bitmap "
                            f"says is free")

    sb = fs.block(blocks[0])
    magic, kind, _ = struct.unpack_from(">III", sb, 0)
    if magic != JBD_MAGIC:
        return None, [f"the journal's first block has no jbd2 magic "
                      f"({magic:#x})"]
    if kind not in (JBD_SUPERBLOCK_V1, JBD_SUPERBLOCK_V2):
        return None, [f"the journal begins with block type {kind}, which is "
                      f"not a superblock"]

    bs, maxlen, first, sequence, start = struct.unpack_from(">IIIII", sb, 12)
    if bs != fs.block_size:
        problems.append(f"the journal says {bs} byte blocks, the filesystem "
                        f"says {fs.block_size}")
    if maxlen > len(blocks):
        problems.append(f"the journal says it is {maxlen} blocks long, and "
                        f"its inode has {len(blocks)}")
    if not 1 <= first < max(maxlen, 2):
        problems.append(f"the journal's first usable block is {first}, which "
                        f"is not inside a log of {maxlen}")

    dirty = start != 0
    wants_recovery = bool(fs.incompat & INCOMPAT_RECOVER)

    # the two of them are one sentence said twice, and a disk that says
    # it both ways is a disk somebody can trust. saying "I am fine" over
    # a log that still holds something is the quiet failure: the next
    # driver to mount it skips the replay and reads the version of every
    # block the log was in the middle of correcting
    if dirty and not wants_recovery:
        problems.append(f"the journal holds something (start {start}) and "
                        f"the superblock does not ask to be recovered")

    state = "clean" if not dirty else f"holds something, from block {first}"
    if wants_recovery:
        state += ", and the superblock asks to be recovered"
    return (f"journal: inode {ino}, {maxlen} blocks of {bs}, "
            f"sequence {sequence}, {state}"), problems


def check(fs, verbose):
    problems = []
    owner = {}                  # block -> the inode that claims it
    seen_inodes = set()
    names_of = {}               # inode -> how many names point at it

    def walk(ino, path, expect_parent):
        if ino in seen_inodes:
            return
        seen_inodes.add(ino)

        if not fs.inode_in_use(ino):
            problems.append(f"{path}: inode {ino} is named but marked free")
            return

        d = fs.inode(ino)
        kind = d["mode"] & S_IFMT

        for b in fs.blocks_of(d, want_meta=True):
            if b >= fs.blocks_count:
                problems.append(f"{path}: block {b} is past the end of the "
                                f"image ({fs.blocks_count} blocks)")
                continue
            if not fs.block_in_use(b):
                problems.append(f"{path}: uses block {b}, which the bitmap "
                                f"says is free")
            if b in owner:
                problems.append(f"{path}: block {b} is also claimed by "
                                f"{owner[b]}")
            owner[b] = path

        if kind == S_IFDIR:
            entries = fs.readdir(d)
            names = [n for n, _, _ in entries]
            if "." not in names:
                problems.append(f"{path}: no . entry")
            if ".." not in names:
                problems.append(f"{path}: no .. entry")

            for name, child, ftype in entries:
                if name == ".":
                    if child != ino:
                        problems.append(f"{path}: . points at {child}, not "
                                        f"{ino}")
                    continue
                if name == "..":
                    if expect_parent is not None and child != expect_parent:
                        problems.append(f"{path}: .. points at {child}, but "
                                        f"its parent is {expect_parent}")
                    continue

                names_of[child] = names_of.get(child, 0) + 1
                sub = path.rstrip("/") + "/" + name
                cd = fs.inode(child)
                ck = cd["mode"] & S_IFMT
                want = {S_IFREG: 1, S_IFDIR: 2, S_IFLNK: 7}.get(ck, 0)
                if fs.incompat & 0x2 and ftype != want:
                    problems.append(f"{sub}: the directory says type "
                                    f"{ftype}, the inode says {want}")
                if ck == S_IFDIR:
                    walk(child, sub, ino)
                else:
                    walk(child, sub, None)
                    if verbose:
                        note = ""
                        if ck == S_IFLNK:
                            note = " -> " + fs.read_file(cd).decode(
                                errors="replace")
                        print(f"  {oct(cd['mode'] & 0o7777)[2:]:>4} "
                              f"{cd['uid']}:{cd['gid']} "
                              f"{cd['size']:>8}  {sub}{note}")
        elif verbose and kind == S_IFDIR:
            pass

    walk(2, "/", 2)

    # the counts everything else is allowed to trust
    free = sum(1 for b in range(fs.first_data_block, fs.blocks_count)
               if not fs.block_in_use(b))
    if free != fs.free_blocks:
        problems.append(f"superblock says {fs.free_blocks} free blocks, the "
                        f"bitmaps say {free}")

    free_i = sum(1 for i in range(1, fs.inodes_count + 1)
                 if not fs.inode_in_use(i))
    if free_i != fs.free_inodes:
        problems.append(f"superblock says {fs.free_inodes} free inodes, the "
                        f"bitmaps say {free_i}")

    per_group = sum(g["free_blocks"] for g in fs.gd)
    if per_group != fs.free_blocks:
        problems.append(f"the group descriptors add up to {per_group} free "
                        f"blocks, the superblock says {fs.free_blocks}")

    for ino, count in names_of.items():
        d = fs.inode(ino)
        if (d["mode"] & S_IFMT) == S_IFDIR:
            continue        # directories count their children too
        if d["links"] != count:
            problems.append(f"inode {ino} says {d['links']} links, but "
                            f"{count} names point at it")

    # an inode marked in use that nothing names is a leak
    for i in range(fs.first_ino, fs.inodes_count + 1):
        if fs.inode_in_use(i) and i not in seen_inodes:
            problems.append(f"inode {i} is marked in use but nothing names it")

    return problems


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    with open(sys.argv[1], "rb") as f:
        raw = f.read()

    verbose = "--tree" in sys.argv
    want = None
    if "--part" in sys.argv:
        want = int(sys.argv[sys.argv.index("--part") + 1])

    try:
        raw, how = find_ext2(raw, want)
        fs = Ext2(raw)
    except Bad as e:
        print(f"not readable: {e}")
        return 1

    if how != "the whole image":
        print(f"reading {how}")

    if "--cat" in sys.argv:
        want = sys.argv[sys.argv.index("--cat") + 1]
        ino = 2
        for part in want.strip("/").split("/"):
            if not part:
                continue
            found = None
            for name, child, _ in fs.readdir(fs.inode(ino)):
                if name == part:
                    found = child
                    break
            if found is None:
                print(f"no such path: {want}")
                return 1
            ino = found
        sys.stdout.buffer.write(fs.read_file(fs.inode(ino)))
        return 0

    print(f"{'ext4' if fs.incompat & INCOMPAT_EXTENTS else 'ext2'} rev {fs.rev}, {fs.block_size} byte blocks, "
          f"{fs.blocks_count} blocks in {fs.groups} group(s), "
          f"{fs.inodes_count} inodes, labelled \"{fs.label}\"")
    if verbose:
        print("  mode owner     size  path")

    said, journal_problems = journal(fs)
    if said is not None:
        print(said)

    problems = journal_problems + check(fs, verbose)

    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems[:40]:
            print(f"  {p}")
        if len(problems) > 40:
            print(f"  ... and {len(problems) - 40} more")
        return 1

    print("nothing wrong with it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
