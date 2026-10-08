#!/usr/bin/env python3
"""build an ext2 filesystem image out of a directory.

there is no mke2fs on this machine, so this is it. that is worth saying
plainly, because it means the formatter and the driver share an author
and can therefore agree on something wrong, which is exactly what
happened to mkfat.py in 0.1.10 and took an independently written reader
to find. tools/readext2.py is that reader here: written from the layout
rather than from this file, and run against every image the build makes.

what is produced is ext2 revision 1 with 1 KiB blocks, 128-byte inodes,
and one feature flag: FILETYPE, which puts the kind of a thing in its
directory entry so that listing a directory does not mean reading an
inode per name. no journal (that is ext3), no extents (that is ext4), no
sparse_super, no dir_index. anything this does not implement is left
switched off in the superblock rather than half-done.
"""

import os
import struct
import sys
import time

BLOCK_SIZE = 1024
INODE_SIZE = 128
BLOCKS_PER_GROUP = BLOCK_SIZE * 8      # one block of bitmap covers this
INODES_PER_GROUP = 512
FIRST_INO = 11                          # 1..10 are reserved by the format
FIRST_DATA_BLOCK = 1                    # block 0 is the boot block
ROOT_INO = 2

EXT2_MAGIC = 0xEF53
INCOMPAT_FILETYPE = 0x0002

S_IFREG = 0x8000
S_IFDIR = 0x4000
S_IFLNK = 0xA000

FT_REG, FT_DIR, FT_LNK = 1, 2, 7

# 12 direct, then one indirect, then one double indirect. 1024/4 = 256
# pointers per block, so that reaches 12 + 256 + 65536 blocks, which is
# 64 MiB and a bit, comfortably more than this image will ever be
DIRECT = 12
PER_BLOCK = BLOCK_SIZE // 4


class Image:
    def __init__(self, total_blocks):
        self.total_blocks = total_blocks
        self.groups = ((total_blocks - FIRST_DATA_BLOCK) + BLOCKS_PER_GROUP - 1) \
            // BLOCKS_PER_GROUP
        self.data = bytearray(total_blocks * BLOCK_SIZE)

        self.inodes_count = self.groups * INODES_PER_GROUP
        self.inode_table_blocks = (INODES_PER_GROUP * INODE_SIZE) // BLOCK_SIZE

        # the descriptor table, however many blocks that takes
        self.gdt_blocks = (self.groups * 32 + BLOCK_SIZE - 1) // BLOCK_SIZE

        # what is in use. every group starts with its own metadata taken
        self.block_used = bytearray(total_blocks)
        self.inode_used = bytearray(self.inodes_count + 1)

        # block 0 is the boot block and is nobody's
        self.block_used[0] = 1

        self.group_first_data = []
        for g in range(self.groups):
            # with 1 KiB blocks the first data block is 1, not 0, block
            # zero is the boot block and belongs to no group at all. so
            # group g starts at 1 + g*blocks_per_group, and getting this
            # off by one puts every bitmap one block away from what it
            # describes
            base = FIRST_DATA_BLOCK + g * BLOCKS_PER_GROUP
            at = base
            take = []
            # in a non-sparse filesystem every group carries a copy of
            # the superblock and the descriptor table
            take.append(at)
            at += 1
            for _ in range(self.gdt_blocks):
                take.append(at)
                at += 1
            self.block_bitmap_at = None
            take.append(at); block_bitmap = at; at += 1
            take.append(at); inode_bitmap = at; at += 1
            for _ in range(self.inode_table_blocks):
                take.append(at)
                at += 1

            for b in take:
                if b < total_blocks:
                    self.block_used[b] = 1

            self.group_first_data.append(
                (block_bitmap, inode_bitmap, at, at))


    def alloc_block(self):
        for b in range(self.total_blocks):
            if not self.block_used[b]:
                self.block_used[b] = 1
                self.write_block(b, bytes(BLOCK_SIZE))
                return b
        raise RuntimeError("image is full of blocks")

    def alloc_inode(self):
        for i in range(FIRST_INO, self.inodes_count + 1):
            if not self.inode_used[i]:
                self.inode_used[i] = 1
                return i
        raise RuntimeError("image is full of inodes")


    def write_block(self, n, data):
        assert len(data) <= BLOCK_SIZE
        at = n * BLOCK_SIZE
        self.data[at:at + BLOCK_SIZE] = data.ljust(BLOCK_SIZE, b"\0")

    def read_block(self, n):
        at = n * BLOCK_SIZE
        return bytes(self.data[at:at + BLOCK_SIZE])

    def inode_location(self, ino):
        g = (ino - 1) // INODES_PER_GROUP
        idx = (ino - 1) % INODES_PER_GROUP
        table = self.group_first_data[g][2] - self.inode_table_blocks
        return table * BLOCK_SIZE + idx * INODE_SIZE

    def write_inode(self, ino, raw):
        at = self.inode_location(ino)
        self.data[at:at + INODE_SIZE] = raw.ljust(INODE_SIZE, b"\0")


def pack_inode(mode, uid, gid, size, links, blocks_512, block_ptrs, when,
               inline=None):
    raw = bytearray(INODE_SIZE)
    struct.pack_into("<H", raw, 0, mode)
    struct.pack_into("<H", raw, 2, uid)
    struct.pack_into("<I", raw, 4, size)
    struct.pack_into("<I", raw, 8, when)     # atime
    struct.pack_into("<I", raw, 12, when)    # ctime
    struct.pack_into("<I", raw, 16, when)    # mtime
    struct.pack_into("<I", raw, 20, 0)       # dtime
    struct.pack_into("<H", raw, 24, gid)
    struct.pack_into("<H", raw, 26, links)
    struct.pack_into("<I", raw, 28, blocks_512)

    if inline is not None:
        # a "fast symlink": the target lives in the block pointers
        # themselves, because a path shorter than sixty bytes is not
        # worth a whole block
        raw[40:40 + len(inline)] = inline
    else:
        for i, b in enumerate(block_ptrs[:15]):
            struct.pack_into("<I", raw, 40 + i * 4, b)
    return bytes(raw)


class Builder:
    def __init__(self, img):
        self.img = img
        self.now = int(time.time())

    def write_file_blocks(self, data):
        """lay bytes out and return (block pointers, blocks used)"""
        needed = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
        blocks = []
        for i in range(needed):
            b = self.img.alloc_block()
            self.img.write_block(b, data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE])
            blocks.append(b)

        ptrs = [0] * 15
        used = len(blocks)

        for i in range(min(DIRECT, len(blocks))):
            ptrs[i] = blocks[i]

        rest = blocks[DIRECT:]
        if rest:
            ind = self.img.alloc_block()
            used += 1
            ptrs[12] = ind
            first = rest[:PER_BLOCK]
            self.img.write_block(ind, b"".join(
                struct.pack("<I", b) for b in first))
            rest = rest[PER_BLOCK:]

        if rest:
            dind = self.img.alloc_block()
            used += 1
            ptrs[13] = dind
            table = []
            while rest:
                chunk = rest[:PER_BLOCK]
                rest = rest[PER_BLOCK:]
                ind = self.img.alloc_block()
                used += 1
                table.append(ind)
                self.img.write_block(ind, b"".join(
                    struct.pack("<I", b) for b in chunk))
            self.img.write_block(dind, b"".join(
                struct.pack("<I", b) for b in table))

        if rest:
            raise RuntimeError("that file needs triple indirection")

        return ptrs, used

    def make_file(self, data, mode, uid, gid):
        ptrs, used = self.write_file_blocks(data)
        ino = self.img.alloc_inode()
        self.img.write_inode(ino, pack_inode(
            S_IFREG | mode, uid, gid, len(data), 1,
            used * (BLOCK_SIZE // 512), ptrs, self.now))
        return ino

    def make_symlink(self, target, uid, gid):
        ino = self.img.alloc_inode()
        raw = target.encode()
        if len(raw) < 60:
            self.img.write_inode(ino, pack_inode(
                S_IFLNK | 0o777, uid, gid, len(raw), 1, 0, [], self.now,
                inline=raw))
        else:
            ptrs, used = self.write_file_blocks(raw)
            self.img.write_inode(ino, pack_inode(
                S_IFLNK | 0o777, uid, gid, len(raw), 1,
                used * (BLOCK_SIZE // 512), ptrs, self.now))
        return ino

    def write_dir(self, ino, entries, parent_ino, mode, uid, gid):
        """fill in a directory whose inode number is already decided.

        already decided, because its children need to name it in their
        `..` and it needs to name them, so the number has to exist
        before either half is written. doing it the other way round
        means renumbering afterwards, which is how the first version of
        this treated every subdirectory as if it were the root."""
        full = [(".", ino, FT_DIR), ("..", parent_ino, FT_DIR)] + entries

        # a directory is a sequence of blocks, each entirely covered by
        # its entries, the last one's rec_len stretches to the end of
        # the block, which is what makes deleting an entry a matter of
        # widening the one before it
        blocks = []
        cur = bytearray()
        for name, target, ft in full:
            raw = name.encode()
            need = (8 + len(raw) + 3) & ~3
            if len(cur) + need > BLOCK_SIZE:
                blocks.append(cur)
                cur = bytearray()
            entry = bytearray(need)
            struct.pack_into("<I", entry, 0, target)
            struct.pack_into("<H", entry, 4, need)
            entry[6] = len(raw)
            entry[7] = ft
            entry[8:8 + len(raw)] = raw
            cur += entry
        blocks.append(cur)

        ptrs = [0] * 15
        if len(blocks) > DIRECT:
            raise RuntimeError("that directory needs indirect blocks")

        for i, contents in enumerate(blocks):
            # stretch the last entry of each block to fill it
            contents = bytearray(contents)
            at = 0
            last = 0
            while at < len(contents):
                last = at
                at += struct.unpack_from("<H", contents, at + 4)[0]
            struct.pack_into("<H", contents, last + 4, BLOCK_SIZE - last)

            b = self.img.alloc_block()
            self.img.write_block(b, bytes(contents))
            ptrs[i] = b

        links = 2 + sum(1 for _, _, ft in entries if ft == FT_DIR)
        self.img.write_inode(ino, pack_inode(
            S_IFDIR | mode, uid, gid, len(blocks) * BLOCK_SIZE, links,
            len(blocks) * (BLOCK_SIZE // 512), ptrs, self.now))
        return ino

    def reserve(self, ino=None):
        """claim an inode number without writing anything into it yet.
        the root is always number 2, so it cannot come from the
        ordinary allocator at all"""
        if ino is None:
            return self.img.alloc_inode()
        self.img.inode_used[ino] = 1
        return ino


def build_tree(builder, path, ino, parent_ino):
    """fill in the directory `ino` from `path`, recursively.

    the inode is passed in rather than returned, because a directory has
    to know its own number before its children can name it in `..` and
    before it can name them. every subdirectory is reserved here and
    filled in by the call below."""
    entries = []

    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        st = os.lstat(full)
        mode = st.st_mode & 0o7777

        if os.path.islink(full):
            child = builder.make_symlink(os.readlink(full), 0, 0)
            entries.append((name, child, FT_LNK))
        elif os.path.isdir(full):
            child = builder.reserve()
            build_tree(builder, full, child, ino)
            entries.append((name, child, FT_DIR))
        else:
            with open(full, "rb") as f:
                data = f.read()
            child = builder.make_file(data, mode, 0, 0)
            entries.append((name, child, FT_REG))

    builder.write_dir(ino, entries, parent_ino, 0o755, 0, 0)


def finish(img, label):
    """bitmaps, group descriptors, superblock"""
    free_blocks_total = 0
    free_inodes_total = 0
    gdt = bytearray()

    for g in range(img.groups):
        block_bitmap, inode_bitmap, table_end, _ = img.group_first_data[g]
        inode_table = table_end - img.inode_table_blocks

        base = FIRST_DATA_BLOCK + g * BLOCKS_PER_GROUP
        bits = bytearray(BLOCK_SIZE)
        free_blocks = 0
        for i in range(BLOCKS_PER_GROUP):
            b = base + i
            if b >= img.total_blocks:
                bits[i // 8] |= 1 << (i % 8)     # past the end: never free
            elif img.block_used[b]:
                bits[i // 8] |= 1 << (i % 8)
            else:
                free_blocks += 1
        img.write_block(block_bitmap, bytes(bits))

        ibits = bytearray(BLOCK_SIZE)
        free_inodes = 0
        for i in range(INODES_PER_GROUP):
            ino = g * INODES_PER_GROUP + i + 1
            if img.inode_used[ino]:
                ibits[i // 8] |= 1 << (i % 8)
            else:
                free_inodes += 1
        img.write_block(inode_bitmap, bytes(ibits))

        dirs = 0
        for i in range(INODES_PER_GROUP):
            ino = g * INODES_PER_GROUP + i + 1
            if not img.inode_used[ino]:
                continue
            at = img.inode_location(ino)
            mode = struct.unpack_from("<H", img.data, at)[0]
            if mode & 0xF000 == S_IFDIR:
                dirs += 1

        free_blocks_total += free_blocks
        free_inodes_total += free_inodes

        d = bytearray(32)
        struct.pack_into("<I", d, 0, block_bitmap)
        struct.pack_into("<I", d, 4, inode_bitmap)
        struct.pack_into("<I", d, 8, inode_table)
        struct.pack_into("<H", d, 12, free_blocks)
        struct.pack_into("<H", d, 14, free_inodes)
        struct.pack_into("<H", d, 16, dirs)
        gdt += d

    sb = bytearray(BLOCK_SIZE)
    struct.pack_into("<I", sb, 0, img.inodes_count)
    struct.pack_into("<I", sb, 4, img.total_blocks)
    struct.pack_into("<I", sb, 8, 0)                    # reserved for root
    struct.pack_into("<I", sb, 12, free_blocks_total)
    struct.pack_into("<I", sb, 16, free_inodes_total)
    struct.pack_into("<I", sb, 20, 1)                   # first data block
    struct.pack_into("<I", sb, 24, 0)                   # log block size: 1KiB
    struct.pack_into("<I", sb, 28, 0)
    struct.pack_into("<I", sb, 32, BLOCKS_PER_GROUP)
    struct.pack_into("<I", sb, 36, BLOCKS_PER_GROUP)
    struct.pack_into("<I", sb, 40, INODES_PER_GROUP)
    struct.pack_into("<I", sb, 44, int(time.time()))
    struct.pack_into("<I", sb, 48, int(time.time()))
    struct.pack_into("<H", sb, 52, 0)
    struct.pack_into("<H", sb, 54, 0xFFFF)
    struct.pack_into("<H", sb, 56, EXT2_MAGIC)
    struct.pack_into("<H", sb, 58, 1)                   # clean
    struct.pack_into("<H", sb, 60, 1)                   # errors: continue
    struct.pack_into("<H", sb, 62, 0)
    struct.pack_into("<I", sb, 64, int(time.time()))
    struct.pack_into("<I", sb, 68, 0)
    struct.pack_into("<I", sb, 72, 0)                   # linux
    struct.pack_into("<I", sb, 76, 1)                   # dynamic rev
    struct.pack_into("<H", sb, 80, 0)
    struct.pack_into("<H", sb, 82, 0)
    struct.pack_into("<I", sb, 84, FIRST_INO)
    struct.pack_into("<H", sb, 88, INODE_SIZE)
    struct.pack_into("<H", sb, 90, 0)
    struct.pack_into("<I", sb, 92, 0)                   # compat: none
    struct.pack_into("<I", sb, 96, INCOMPAT_FILETYPE)
    struct.pack_into("<I", sb, 100, 0)                  # ro_compat: none
    name = label.encode()[:15]
    sb[120:120 + len(name)] = name

    # the primary, and a copy in every group. no sparse_super here, so
    # every group really does carry one
    img.write_block(1, bytes(sb))
    for g in range(img.groups):
        at = img.group_first_data[g][0] - 1 - img.gdt_blocks
        if g == 0:
            at = 1
        copy = bytearray(sb)
        struct.pack_into("<H", copy, 90, g)
        img.write_block(at, bytes(copy))
        for i in range(img.gdt_blocks):
            img.write_block(at + 1 + i,
                            bytes(gdt[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]))


def main():
    if len(sys.argv) < 4:
        print("usage: mkext2.py <image> <directory> <megabytes> [label]")
        return 1

    out, root, mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
    label = sys.argv[4] if len(sys.argv) > 4 else "velvetos"

    total_blocks = (mb * 1024 * 1024) // BLOCK_SIZE
    img = Image(total_blocks)
    builder = Builder(img)

    builder.reserve(ROOT_INO)
    if os.path.isdir(root):
        build_tree(builder, root, ROOT_INO, ROOT_INO)
    else:
        builder.write_dir(ROOT_INO, [], ROOT_INO, 0o755, 0, 0)

    finish(img, label)

    with open(out, "wb") as f:
        f.write(img.data)

    used = sum(img.block_used)
    print(f"{out}: {mb} MiB, {total_blocks} blocks of {BLOCK_SIZE}, "
          f"{img.groups} group(s), {used} blocks used, "
          f"{sum(img.inode_used)} inodes used")
    return 0


if __name__ == "__main__":
    sys.exit(main())
