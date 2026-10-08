#!/usr/bin/env python3
"""read a fat32 image, and complain about it.

a second opinion, for the reason readext2.py gives: the formatter and
the driver are two programs by one author, and two programs by one
author agreeing is not a check, it is an echo. that has already cost
this project once, mkfat.py and the fat32 parser agreed on a cluster
count that was wrong, and nothing but a reader written apart from both
would have found it.

it matters more in 0.3.21 than it did before. the driver could always
*read* a long name and always refused to write one; now it mints them,
and a long name written slightly wrong is the worst bug this filesystem
can have. it reads back perfectly here, the same code that wrote it is
the code reading it, and is a different file, or no file at all, on
every other machine the disk is ever plugged into.

so nothing here imports mkfat.py or shares a line with it. it walks the
image the way the specification says to, and checks what a fat
filesystem is allowed to assume about itself:

  the boot sector says fat32, and the backup at sector 6 agrees with it
  every copy of the table holds the same thing
  the cluster count and the size of the table agree with each other
  every chain ends, stays in range, and belongs to exactly one file
  a file's chain is exactly as long as its size needs
  every long name is complete, in order, and checksummed to the short
    entry standing behind it
  no entry hides behind the zero byte that ends a directory
  no two entries in one directory share a name, long or short
  every directory has . and .., and .. really is its parent

usage:
  readfat.py <image>             check it, and say what is wrong
  readfat.py <image> --fresh     also insist the free count is right,
                                 which only a just-built image owes
  readfat.py <image> --tree      also print the tree it found
  readfat.py <image> --cat PATH  print the contents of one path
"""

import struct
import sys

ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_LFN = 0x0f

# the thirteen characters of a long entry, at the three runs of bytes
# that were left over once every other field had its place
LFN_AT = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)

EOC = 0x0ffffff8
BAD_CLUSTER = 0x0ffffff7


class Bad(Exception):
    pass


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def lfn_checksum(short):
    total = 0
    for byte in short:
        total = ((((total & 1) << 7) | (total >> 1)) + byte) & 0xff
    return total


def render_short(entry):
    """the eleven bytes as a name, with the two case bits applied,
    which is the whole of what an entry without a long name in front of
    it can say about itself"""
    base = entry[0:8].decode('latin-1').rstrip(' ')
    ext = entry[8:11].decode('latin-1').rstrip(' ')
    if entry[12] & 0x08:
        base = base.lower()
    if entry[12] & 0x10:
        ext = ext.lower()
    return base + '.' + ext if ext else base


class Fat32:
    def __init__(self, raw):
        self.raw = raw
        if len(raw) < 512:
            raise Bad("shorter than a boot sector")

        b = raw
        if u16(b, 510) != 0xaa55:
            raise Bad("no 0x55aa at the end of the boot sector")

        self.sector = u16(b, 11)
        if self.sector not in (512, 1024, 2048, 4096):
            raise Bad(f"{self.sector} bytes to a sector")
        self.spc = u8(b, 13)
        if self.spc == 0 or self.spc & (self.spc - 1):
            raise Bad(f"{self.spc} sectors to a cluster, which is not a power"
                      " of two")

        self.reserved = u16(b, 14)
        self.num_fats = u8(b, 16)
        self.root_entries = u16(b, 17)
        self.fat16_sectors = u16(b, 22)
        self.total = u32(b, 32) or u16(b, 19)
        self.fat_sectors = u32(b, 36)
        self.root_cluster = u32(b, 44)
        self.fsinfo_at = u16(b, 48)
        self.backup_at = u16(b, 50)
        self.label = b[71:82].decode('latin-1').rstrip()
        self.fstype = b[82:90].decode('latin-1')

        if self.reserved == 0 or self.num_fats == 0 or self.fat_sectors == 0:
            raise Bad("the boot sector describes no filesystem at all")
        if len(raw) < self.total * self.sector:
            raise Bad(f"says {self.total} sectors, the file holds "
                      f"{len(raw) // self.sector}")

        self.first_data = self.reserved + self.num_fats * self.fat_sectors
        self.clusters = (self.total - self.first_data) // self.spc
        self.cluster_bytes = self.spc * self.sector
        if self.clusters <= 0:
            raise Bad("no room for any data at all")


    def fat(self, cluster, copy=0):
        at = (self.reserved + copy * self.fat_sectors) * self.sector \
            + cluster * 4
        return u32(self.raw, at) & 0x0fffffff

    def cluster(self, n):
        at = (self.first_data + (n - 2) * self.spc) * self.sector
        return self.raw[at:at + self.cluster_bytes]

    def in_range(self, n):
        return 2 <= n < self.clusters + 2

    def chain(self, start, complain=None):
        """the clusters of one file, in order. a chain that leaves the
        disk or turns back on itself stops here rather than hanging"""
        out = []
        seen = set()
        n = start
        while self.in_range(n):
            if n in seen:
                if complain:
                    complain(f"cluster {n} is its own successor")
                break
            seen.add(n)
            out.append(n)
            nxt = self.fat(n)
            if nxt >= EOC:
                return out
            if nxt == BAD_CLUSTER or not self.in_range(nxt):
                if complain:
                    complain(f"the chain runs off the disk at cluster {n}"
                             f" (which points at {nxt:#x})")
                break
            n = nxt
        return out

    def read_file(self, first, size):
        out = b''
        for c in self.chain(first):
            out += self.cluster(c)
        return out[:size]


class Check:
    def __init__(self, fs, fresh=False):
        self.fs = fs
        self.fresh = fresh
        self.problems = []
        self.notes = []
        self.owner = {}         # cluster -> the path that claims it
        self.tree = []          # (path, size, is_dir)

    def bad(self, what):
        self.problems.append(what)


    def geometry(self):
        fs = self.fs
        if fs.fstype.rstrip() != 'FAT32':
            self.bad(f'the boot sector calls this "{fs.fstype.strip()}"')
        if fs.root_entries != 0:
            self.bad(f"{fs.root_entries} root entries, and fat32 has none --"
                     " its root is a chain like any other")
        if fs.fat16_sectors != 0:
            self.bad("the fat16 table size is set, which no fat32 reader"
                     " looks at and every fat16 one does")
        if fs.clusters < 65525:
            self.bad(f"{fs.clusters} clusters, which is fat16's range: a"
                     " driver going by the count alone would read this as"
                     " the wrong filesystem")
        if not fs.in_range(fs.root_cluster):
            self.bad(f"the root is cluster {fs.root_cluster}, which is not"
                     " a cluster")

        # the one that bit this project before: the geometry can imply
        # more clusters than the table has room to describe, and the
        # chain of the last of them is then read out of the next copy
        entries = fs.fat_sectors * fs.sector // 4
        if entries < fs.clusters + 2:
            self.bad(f"{fs.clusters} clusters but only {entries} entries in"
                     " the table: the last ones have nowhere to record a"
                     " chain")

        if fs.backup_at:
            at = fs.backup_at * fs.sector
            if fs.raw[at:at + 512] != fs.raw[0:512]:
                self.bad(f"the backup boot sector at {fs.backup_at} is not a"
                         " copy of the one at 0")

        first = fs.fat(0)
        if first & 0xff != u8(fs.raw, 21):
            self.bad("the first entry of the table does not hold the media"
                     " descriptor")
        if fs.fat(1) < EOC:
            self.bad("the second entry of the table is not an end mark")

        for copy in range(1, fs.num_fats):
            a = fs.reserved * fs.sector
            b = (fs.reserved + copy * fs.fat_sectors) * fs.sector
            n = fs.fat_sectors * fs.sector
            if fs.raw[a:a + n] != fs.raw[b:b + n]:
                self.bad(f"copy {copy} of the table differs from copy 0")

    def fsinfo(self):
        fs = self.fs
        if not fs.fsinfo_at:
            return
        at = fs.fsinfo_at * fs.sector
        info = fs.raw[at:at + fs.sector]
        if info[0:4] != b'RRaA' or info[484:488] != b'rrAa':
            self.bad("the fsinfo sector is not one")
            return
        claimed = u32(info, 488)
        if claimed == 0xffffffff:
            return              # "I do not know", which is always honest
        free = sum(1 for c in range(2, fs.clusters + 2) if fs.fat(c) == 0)
        if claimed == free:
            return

        # a cache, and the specification says so: every reader is
        # expected to work without it and fsck is expected to correct
        # it. so a disk that has been *written to* since it was made is
        # allowed to have it stale, this driver does not maintain it,
        # which is a decision and not an oversight, and 0.3.22 is the
        # version that owes the disk better. a disk fresh out of a
        # formatter owes it now, and gets asked
        what = (f"fsinfo says {claimed} clusters are free and {free} are")
        if self.fresh:
            self.bad(what + ": a formatter is the one thing that has no"
                     " excuse for leaving this wrong")
        else:
            self.notes.append(what + ", stale, which a disk that has been"
                              " written to is allowed to be")

    def claim(self, first, size, path, is_dir):
        fs = self.fs
        if first == 0:
            if size and not is_dir:
                self.bad(f"{path}: {size} bytes and no clusters to hold them")
            return []

        chain = fs.chain(first, lambda w: self.bad(f"{path}: {w}"))
        for c in chain:
            if c in self.owner:
                self.bad(f"{path}: cluster {c} is also claimed by"
                         f" {self.owner[c]}")
            else:
                self.owner[c] = path
        if not is_dir:
            need = (size + fs.cluster_bytes - 1) // fs.cluster_bytes
            if len(chain) != need:
                self.bad(f"{path}: {size} bytes wants {need} cluster(s), the"
                         f" chain holds {len(chain)}")
        return chain


    def readdir(self, cluster, path):
        """the entries of one directory, with every structural
        complaint the walk can make on the way past"""
        fs = self.fs
        entries = []            # (name, short, attr, first, size)
        pieces, want, checksum = {}, None, None
        ended = False

        for c in fs.chain(cluster):
            data = fs.cluster(c)
            for off in range(0, len(data), 32):
                e = data[off:off + 32]

                if e[0] == 0x00:
                    ended = True
                    continue
                if ended:
                    self.bad(f"{path}: an entry at cluster {c}+{off} sits"
                             " behind the zero byte that ends the directory,"
                             " where no reader will ever look")
                    ended = False       # said once is enough
                if e[0] == 0xe5:
                    pieces, want, checksum = {}, None, None
                    continue

                attr = e[11]
                if attr & 0x3f == ATTR_LFN:
                    index = e[0] & 0x1f
                    if index == 0 or index > 20:
                        self.bad(f"{path}: a long entry numbered {index}")
                        pieces, want = {}, None
                        continue
                    if e[0] & 0x40:
                        pieces, want, checksum = {}, index, e[13]
                    elif want is None or e[13] != checksum:
                        self.bad(f"{path}: a long entry belonging to no name"
                                 ", either the piece marked last is missing"
                                 " or the checksums disagree")
                        pieces, want = {}, None
                        continue
                    if e[26:28] != b'\x00\x00':
                        self.bad(f"{path}: a long entry with a cluster number"
                                 " in it")
                    pieces[index] = bytes(e)
                    continue

                if attr & ATTR_VOLUME_ID:
                    if path != '/':
                        self.bad(f"{path}: a volume label outside the root")
                    pieces, want, checksum = {}, None, None
                    continue

                name = render_short(e)
                if want is not None:
                    name = self.assemble(pieces, want, e, name, path)
                pieces, want, checksum = {}, None, None

                entries.append((name, bytes(e[0:11]), attr,
                                (u16(e, 20) << 16) | u16(e, 26), u32(e, 28)))

        if want is not None:
            self.bad(f"{path}: long entries at the end of the directory with"
                     " no entry behind them to name")

        self.no_duplicates(entries, path)
        return entries

    def assemble(self, pieces, want, entry, fallback, path):
        """the long name, out of its pieces, or a complaint and the
        short name, which is what a reader that does not trust the
        pieces would show, and so is what the file would be called"""
        if lfn_checksum(entry[0:11]) != pieces[max(pieces)][13]:
            self.bad(f"{path}: the long name in front of {fallback} is"
                     " checksummed to some other entry, on any other"
                     " machine this file is called something else")
            return fallback
        if sorted(pieces) != list(range(1, want + 1)):
            missing = [i for i in range(1, want + 1) if i not in pieces]
            self.bad(f"{path}: the long name in front of {fallback} is"
                     f" missing piece(s) {missing}")
            return fallback

        out = ''
        for i in range(1, want + 1):
            e = pieces[i]
            for at in LFN_AT:
                c = u16(e, at)
                if c in (0x0000, 0xffff):
                    continue
                out += chr(c)
        if not out:
            self.bad(f"{path}: an empty long name in front of {fallback}")
            return fallback
        return out

    def no_duplicates(self, entries, path):
        seen, shorts = {}, {}
        for name, short, _attr, _first, _size in entries:
            key = name.lower()
            if key in seen:
                self.bad(f"{path}: two entries called {name}")
            seen[key] = True
            if short in shorts:
                self.bad(f"{path}: two entries share the short name"
                         f" {short.decode('latin-1')!r}, which is all an"
                         " older reader of this disk has to tell them apart")
            shorts[short] = name

    def walk(self, cluster, path, parent_cluster):
        entries = self.readdir(cluster, path)

        if path != '/':
            dots = [e for e in entries if e[0] in ('.', '..')]
            if [e[0] for e in dots] != ['.', '..']:
                self.bad(f"{path}: does not begin with . and ..")
            else:
                if dots[0][3] != cluster:
                    self.bad(f"{path}: . points at cluster {dots[0][3]},"
                             f" not at {cluster}")
                up = 0 if parent_cluster == self.fs.root_cluster \
                    else parent_cluster
                if dots[1][3] != up:
                    self.bad(f"{path}: .. points at cluster {dots[1][3]},"
                             f" not at {up}")

        for name, _short, attr, first, size in entries:
            if name in ('.', '..'):
                continue
            here = path.rstrip('/') + '/' + name
            if attr & ATTR_DIRECTORY:
                self.tree.append((here, 0, True))
                self.claim(first, 0, here, True)
                if first and self.fs.in_range(first):
                    self.walk(first, here, cluster)
            else:
                self.tree.append((here, size, False))
                self.claim(first, size, here, False)

    def run(self):
        self.geometry()
        self.fsinfo()
        self.claim(self.fs.root_cluster, 0, '/', True)
        self.walk(self.fs.root_cluster, '/', self.fs.root_cluster)
        return self.problems


def cat(fs, want):
    cluster, size, is_dir = fs.root_cluster, 0, True
    for part in want.strip('/').split('/'):
        if not part:
            continue
        if not is_dir:
            return None
        found = None
        for name, _short, attr, first, sz in Check(fs).readdir(cluster, '/'):
            if name.lower() == part.lower():
                found = (first, sz, bool(attr & ATTR_DIRECTORY))
                break
        if found is None:
            return None
        cluster, size, is_dir = found
    return None if is_dir else fs.read_file(cluster, size)


def main():
    if len(sys.argv) < 2 or sys.argv[1].startswith('-'):
        print(__doc__)
        return 1

    with open(sys.argv[1], 'rb') as f:
        raw = f.read()

    try:
        fs = Fat32(raw)
    except Bad as e:
        print(f"not readable: {e}")
        return 1

    if '--cat' in sys.argv:
        want = sys.argv[sys.argv.index('--cat') + 1]
        data = cat(fs, want)
        if data is None:
            print(f"no such file: {want}")
            return 1
        sys.stdout.buffer.write(data)
        return 0

    check = Check(fs, fresh='--fresh' in sys.argv)
    problems = check.run()

    print(f'fat32 "{fs.label}", {fs.sector}-byte sectors, '
          f'{fs.spc} to a cluster, {fs.clusters} clusters, '
          f'{fs.num_fats} tables of {fs.fat_sectors} sectors')
    if '--tree' in sys.argv:
        for path, size, is_dir in sorted(check.tree):
            print(f"  {'dir ' if is_dir else f'{size:8d}'}  {path}")
    for note in check.notes:
        print(f"  note: {note}")

    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems[:40]:
            print(f"  {p}")
        if len(problems) > 40:
            print(f"  ... and {len(problems) - 40} more")
        return 1

    print("nothing wrong with it")
    return 0


if __name__ == '__main__':
    sys.exit(main())
