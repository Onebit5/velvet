#!/usr/bin/env python3
"""break a filesystem on purpose, and see whether fsck notices.

a checker that agrees about a clean filesystem has agreed about
nothing. every check in `tools/fsck.c` is here as a *break*: an
image is damaged in one specific way, fsck is run over it, and the run
has to find that kind of problem and no other.

and then the half that matters more. fsck does not only report, it
mends what has exactly one right answer. so for those, the image is
mended and handed to `tools/readext4.py`, which shares no line with the
checker or the driver, and which has to call the result clean. that is
the only way to know a repair repaired anything: my checker saying it
fixed something is my checker's opinion of my checker.

the breaks that must *not* be mended are just as important. a block two
files both claim has no right answer, one of them is wrong and
nothing on the disk says which, and an fsck that picks one is an fsck
that destroys data while reporting success.

usage:  tools/fsckcheck.py
"""

import os
import struct
import subprocess
import sys
import tempfile
import shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FSCK = os.path.join(ROOT, "bin", "tests", "fsckext4")
MKEXT4 = os.path.join(ROOT, "bin", "tests", "mkext4")


def run(args):
    return subprocess.run(args, capture_output=True, cwd=ROOT, text=True)


class Image:
    """enough of the layout to damage it precisely.

    written from the on-disk format, like everything else that judges
    this project's filesystems, if it went through the driver it
    could only ever break things the driver already agrees about"""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.raw = bytearray(f.read())

        sb = self.raw[1024:2048]
        self.block_size = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.inodes_count = struct.unpack_from("<I", sb, 0)[0]
        self.blocks_count = struct.unpack_from("<I", sb, 4)[0]
        self.first_data_block = struct.unpack_from("<I", sb, 20)[0]
        self.blocks_per_group = struct.unpack_from("<I", sb, 32)[0]
        self.inodes_per_group = struct.unpack_from("<I", sb, 40)[0]
        self.inode_size = struct.unpack_from("<H", sb, 88)[0]
        self.groups = (self.blocks_count - self.first_data_block
                       + self.blocks_per_group - 1) // self.blocks_per_group
        self.gdt = (self.first_data_block + 1) * self.block_size

    def gd(self, g):
        at = self.gdt + g * 32
        return (struct.unpack_from("<I", self.raw, at)[0],      # block bitmap
                struct.unpack_from("<I", self.raw, at + 4)[0],  # inode bitmap
                struct.unpack_from("<I", self.raw, at + 8)[0])  # inode table

    def inode_at(self, ino):
        g, i = (ino - 1) // self.inodes_per_group, \
               (ino - 1) % self.inodes_per_group
        return self.gd(g)[2] * self.block_size + i * self.inode_size

    def put16(self, at, v):
        struct.pack_into("<H", self.raw, at, v)

    def put32(self, at, v):
        struct.pack_into("<I", self.raw, at, v)

    def flip_block_bit(self, g, bit, on):
        at = self.gd(g)[0] * self.block_size + bit // 8
        if on:
            self.raw[at] |= 1 << (bit % 8)
        else:
            self.raw[at] &= ~(1 << (bit % 8)) & 0xFF

    def first_file_inode(self):
        """an inode that is a regular file with at least one block"""
        for ino in range(11, min(self.inodes_count, 64) + 1):
            at = self.inode_at(ino)
            mode = struct.unpack_from("<H", self.raw, at)[0]
            if (mode & 0xF000) == 0x8000 and \
                    struct.unpack_from("<I", self.raw, at + 4)[0] > 0:
                return ino
        raise RuntimeError("no regular file in the image to damage")

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.raw)
        return path


#
# each returns the kind of problem fsck should report, and whether it is
# one fsck is allowed to mend

def break_super_free_blocks(img):
    img.put32(1024 + 12, img.blocks_count - 1)
    return "a free count that is not the true one", True


def break_group_free_inodes(img):
    at = img.gdt + 14
    img.put16(at, struct.unpack_from("<H", img.raw, at)[0] + 7)
    return "a free count that is not the true one", True


def break_link_count(img):
    ino = img.first_file_inode()
    img.put16(img.inode_at(ino) + 26, 9)
    return "a link count that is not the number of names", True


def break_leaked_block(img):
    """a block marked used that no file claims. the bitmap's last bits
    of a group are past the end of nothing, so a block well inside the
    data area is picked instead, one nothing has allocated yet"""
    bit = img.blocks_per_group - 20
    img.flip_block_bit(0, bit, True)
    return "a block marked used that nothing claims", True


def break_unmarked_block(img):
    """the dangerous direction: a block a file is using that the bitmap
    calls free, which the allocator will hand out again"""
    ino = img.first_file_inode()
    at = img.inode_at(ino)
    flags = struct.unpack_from("<I", img.raw, at + 32)[0]
    if flags & 0x80000:
        # an extent-mapped file: the first run's first block
        block = (struct.unpack_from("<H", img.raw, at + 40 + 12 + 6)[0] << 32) \
            | struct.unpack_from("<I", img.raw, at + 40 + 12 + 8)[0]
    else:
        block = struct.unpack_from("<I", img.raw, at + 40)[0]
    g = (block - img.first_data_block) // img.blocks_per_group
    bit = (block - img.first_data_block) % img.blocks_per_group
    img.flip_block_bit(g, bit, False)
    return "a block in use that the bitmap calls free", True


def break_crosslink(img):
    """two files claiming one block. no right answer exists, so fsck
    must say so and change nothing"""
    a = img.first_file_inode()
    b = None
    for ino in range(a + 1, min(img.inodes_count, 64) + 1):
        at = img.inode_at(ino)
        mode = struct.unpack_from("<H", img.raw, at)[0]
        if (mode & 0xF000) == 0x8000 and \
                struct.unpack_from("<I", img.raw, at + 4)[0] > 0:
            b = ino
            break
    if b is None:
        raise RuntimeError("need two files to cross-link")

    at_a, at_b = img.inode_at(a), img.inode_at(b)
    flags = struct.unpack_from("<I", img.raw, at_a + 32)[0]
    if flags & 0x80000:
        # point b's first extent at a's first block
        img.raw[at_b + 40 + 12 + 6: at_b + 40 + 12 + 12] = \
            img.raw[at_a + 40 + 12 + 6: at_a + 40 + 12 + 12]
        img.put16(at_b + 40 + 12 + 4, 1)
        img.put32(at_b + 40 + 12 + 0, 0)
    else:
        img.put32(at_b + 40, struct.unpack_from("<I", img.raw, at_a + 40)[0])
    return "a block two files both claim", False


def break_dir_record(img):
    """a record whose length walks off the end of its block. a reader
    that trusts it reads the next block as directory entries"""
    at = img.inode_at(2)        # the root
    flags = struct.unpack_from("<I", img.raw, at + 32)[0]
    if flags & 0x80000:
        block = (struct.unpack_from("<H", img.raw, at + 40 + 12 + 6)[0] << 32) \
            | struct.unpack_from("<I", img.raw, at + 40 + 12 + 8)[0]
    else:
        block = struct.unpack_from("<I", img.raw, at + 40)[0]
    img.put16(block * img.block_size + 4, img.block_size + 64)
    return "a directory record that walks off its block", False


def break_extent_magic(img):
    ino = img.first_file_inode()
    at = img.inode_at(ino)
    if not (struct.unpack_from("<I", img.raw, at + 32)[0] & 0x80000):
        return None, False          # an ext2 image: nothing to damage
    img.put16(at + 40, 0x1234)
    return "an extent tree that does not parse", False


def break_orphan_inode(img):
    """an inode marked in use that no name points at. the answer
    everywhere else is lost+found and there is not one, so this must be
    reported and left alone"""
    free = None
    for ino in range(11, img.inodes_count + 1):
        g, i = (ino - 1) // img.inodes_per_group, \
               (ino - 1) % img.inodes_per_group
        at = img.gd(g)[1] * img.block_size + i // 8
        if not (img.raw[at] >> (i % 8)) & 1:
            free = (ino, g, i, at)
            break
    if free is None:
        raise RuntimeError("no free inode to orphan")

    ino, g, i, at = free
    img.raw[at] |= 1 << (i % 8)
    # and make it look like a real file, so it is an orphan rather than
    # an inode full of zeroes
    node = img.inode_at(ino)
    img.put16(node + 0, 0x8000 | 0o644)
    img.put16(node + 26, 1)

    # the counts follow from the bit, and are not what is under test
    gd_at = img.gdt + g * 32 + 14
    img.put16(gd_at, struct.unpack_from("<H", img.raw, gd_at)[0] - 1)
    img.put32(1024 + 16,
              struct.unpack_from("<I", img.raw, 1024 + 16)[0] - 1)
    return "an inode in use that no name reaches", False


BREAKS = [
    ("a free count in the superblock", break_super_free_blocks),
    ("a free count in a group descriptor", break_group_free_inodes),
    ("a link count", break_link_count),
    ("a block marked used that nothing claims", break_leaked_block),
    ("a block in use the bitmap calls free", break_unmarked_block),
    ("two files claiming one block", break_crosslink),
    ("a directory record past the end of its block", break_dir_record),
    ("an extent header with the wrong magic", break_extent_magic),
    ("an inode in use that nothing names", break_orphan_inode),
]


def main():
    if not os.path.exists(FSCK):
        print("  bin/tests/fsckext4 is not built")
        return 1
    if not os.path.exists(MKEXT4):
        print("  bin/tests/mkext4 is not built")
        return 1

    tree = os.path.join(ROOT, "bin", "tests", "ext4root")
    if not os.path.isdir(tree):
        print("  bin/tests/ext4root is not there, `make ext4-image`")
        return 1

    work = tempfile.mkdtemp()
    try:
        return check(work, tree)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check(work, tree):
    good = os.path.join(work, "good.img")
    r = run([MKEXT4, good, tree, "16"])
    if r.returncode != 0:
        print(f"  could not build an image to damage: {r.stderr.strip()[:160]}")
        return 1

    bad = 0
    checked = 0

    # the clean image first. a checker that complains about a good
    # filesystem is worse than one that misses a bad one
    r = run([FSCK, "-n", good])
    if r.returncode != 0 or "nothing wrong with it" not in r.stdout:
        print("  fsck complains about a filesystem it was just handed:")
        for line in r.stdout.splitlines()[:6]:
            print(f"      {line}")
        bad += 1
    checked += 1

    for name, make_break in BREAKS:
        img = Image(good)
        try:
            want, mendable = make_break(img)
        except RuntimeError as e:
            print(f"  {name}: cannot set up, {e}")
            bad += 1
            continue
        if want is None:
            continue

        broken = img.save(os.path.join(work, "broken.img"))

        # first with -n, which must find it and change nothing
        before = open(broken, "rb").read()
        r = run([FSCK, "-n", broken])
        after = open(broken, "rb").read()

        if want not in r.stdout:
            print(f"  {name}: fsck did not report it")
            for line in r.stdout.splitlines()[:6]:
                print(f"      {line}")
            bad += 1
            checked += 1
            continue
        if before != after:
            print(f"  {name}: fsck -n changed the image")
            bad += 1
        checked += 1

        # then for real
        r = run([FSCK, broken])
        mended = "mended" in r.stdout and "(left alone)" not in r.stdout

        if mendable:
            if not mended:
                print(f"  {name}: fsck found it and did not mend it")
                for line in r.stdout.splitlines()[:6]:
                    print(f"      {line}")
                bad += 1
            else:
                # and the closing of the loop: a reader that shares no
                # line with the checker has to call the result clean
                r2 = run(["python3", "tools/readext4.py", broken])
                r3 = run([FSCK, "-n", broken])
                if r2.returncode != 0 \
                        or "nothing wrong with it" not in r2.stdout:
                    print(f"  {name}: mended, and readext4.py still refuses it:")
                    for line in (r2.stdout + r2.stderr).splitlines()[:6]:
                        print(f"      {line}")
                    bad += 1
                elif "nothing wrong with it" not in r3.stdout:
                    print(f"  {name}: mended, and fsck still finds something")
                    bad += 1
        else:
            if mended:
                print(f"  {name}: fsck mended something it cannot know the "
                      f"answer to")
                bad += 1
        checked += 1

    if not bad:
        print(f"  {checked} checks: every break found, and every mend "
              f"agreed with by readext4.py")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
